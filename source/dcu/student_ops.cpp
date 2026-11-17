//-----------------------------------------------------------------------------
//  student_ops.cpp
//
//  7 个 DCU 基础算子的实现。
//    * 定义 USE_HIP 时：真正的 HIP kernel（每行一线程 / 每元素一线程 + 归约）
//    * 未定义 USE_HIP 时：同语义的 CPU 参考实现，用于本地自测与正确性对照
//
//  归约一律走 "shared 内树形归约 + 每块 atomicAdd"，
//  不依赖 warp/wavefront 尺寸假设，gfx936 上同样成立。
//-----------------------------------------------------------------------------
#include "student_ops.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef USE_HIP
#include <hip/hip_runtime.h>

namespace {

// ---------------------------------------------------------------------------
// y = A * x ，A 为 CSR。每线程负责一行。
// ---------------------------------------------------------------------------
__global__ void spmv_csr_kernel(int n, const int* rowPtr, const int* colIdx,
                                const double* vals, const double* x, double* y) {
  int row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= n) return;
  int start = rowPtr[row];
  int end = rowPtr[row + 1];
  double sum = 0.0;
  for (int i = start; i < end; ++i) {
    sum += vals[i] * x[colIdx[i]];
  }
  y[row] = sum;
}

// ---------------------------------------------------------------------------
// out = a*x + b*y
// ---------------------------------------------------------------------------
__global__ void axpby_kernel(int n, double a, const double* x, double b,
                             const double* y, double* out) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) out[i] = a * x[i] + b * y[i];
}

// ---------------------------------------------------------------------------
// out = (alpha * x) .* y
// ---------------------------------------------------------------------------
__global__ void mul_kernel(int n, double alpha, const double* x,
                           const double* y, double* out) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) out[i] = alpha * x[i] * y[i];
}

// ---------------------------------------------------------------------------
// 块内树形归约，结果由 0 号线程 atomicAdd 到 result
// ---------------------------------------------------------------------------
__device__ void blockReduceAdd(double val, double* shared, double* result) {
  int tid = threadIdx.x;
  shared[tid] = val;
  __syncthreads();
  for (int s = blockDim.x / 2; s > 0; s >>= 1) {
    if (tid < s) shared[tid] += shared[tid + s];
    __syncthreads();
  }
  if (tid == 0) atomicAdd(result, shared[0]);
}

// ---------------------------------------------------------------------------
// result += x . y（分块累加）
// ---------------------------------------------------------------------------
__global__ void dot_kernel(int n, const double* x, const double* y,
                           double* result) {
  extern __shared__ double sdata[];
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  double v = (i < n) ? x[i] * y[i] : 0.0;
  blockReduceAdd(v, sdata, result);
}

// ---------------------------------------------------------------------------
// result += x . x ，用于二范数
// ---------------------------------------------------------------------------
__global__ void norm2sq_kernel(int n, const double* x, double* result) {
  extern __shared__ double sdata[];
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  double v = (i < n) ? x[i] * x[i] : 0.0;
  blockReduceAdd(v, sdata, result);
}

// ---------------------------------------------------------------------------
// y = A * x ，A 为 m x n 稠密行主序。每线程负责一行。
// ---------------------------------------------------------------------------
__global__ void gemv_kernel(int m, int n, const double* A, const double* x,
                            double* y) {
  int row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= m) return;
  const double* arow = A + (size_t)row * (size_t)n;
  double sum = 0.0;
  for (int j = 0; j < n; ++j) sum += arow[j] * x[j];
  y[row] = sum;
}

// ---------------------------------------------------------------------------
// C = A * B（CSR）。C 的行结构已给定，每线程负责 C 的一行。
// 对 C 行内每个 (j)，在 A 行的 k 上求和 a(i,k)*b(k,j)；
// B 的行内列号升序，故用二分查找定位 j。
// ---------------------------------------------------------------------------
__global__ void spgemm_kernel(int n, const int* aRowPtr, const int* aColIdx,
                              const double* aVals, const int* bRowPtr,
                              const int* bColIdx, const double* bVals,
                              const int* cRowPtr, const int* cColIdx,
                              double* cVals) {
  int row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= n) return;

  int cStart = cRowPtr[row];
  int cEnd = cRowPtr[row + 1];
  int aStart = aRowPtr[row];
  int aEnd = aRowPtr[row + 1];

  for (int ci = cStart; ci < cEnd; ++ci) {
    int j = cColIdx[ci];
    double sum = 0.0;
    for (int ai = aStart; ai < aEnd; ++ai) {
      int k = aColIdx[ai];
      int lo = bRowPtr[k];
      int hi = bRowPtr[k + 1] - 1;
      while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        int col = bColIdx[mid];
        if (col == j) {
          sum += aVals[ai] * bVals[mid];
          break;
        } else if (col < j) {
          lo = mid + 1;
        } else {
          hi = mid - 1;
        }
      }
    }
    cVals[ci] = sum;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 归约辅助：临时缓冲置 0、启动 kernel、取回标量
// ---------------------------------------------------------------------------
static double reduce_launch(int n, const double* x, const double* y, bool is_dot,
                            student_stream_t stream) {
  const int block = 256;
  int grid = (n + block - 1) / block;
  if (grid < 1) grid = 1;
  size_t shmem = (size_t)block * sizeof(double);

  double* d_sum = 0;
  if (hipMalloc((void**)&d_sum, sizeof(double)) != hipSuccess || !d_sum) {
    return 0.0;
  }
  hipMemsetAsync(d_sum, 0, sizeof(double), stream);

  if (is_dot) {
    dot_kernel<<<grid, block, shmem, stream>>>(n, x, y, d_sum);
  } else {
    norm2sq_kernel<<<grid, block, shmem, stream>>>(n, x, d_sum);
  }

  double h_sum = 0.0;
  hipMemcpyAsync(&h_sum, d_sum, sizeof(double), hipMemcpyDeviceToHost, stream);
  hipStreamSynchronize(stream);
  hipFree(d_sum);
  return h_sum;
}

#else  // ---------------- 未启用 HIP：CPU 参考实现 -------------------------

static double reduce_launch(int n, const double* x, const double* y, bool is_dot,
                            student_stream_t) {
  double s = 0.0;
  for (int i = 0; i < n; ++i) s += is_dot ? (x[i] * y[i]) : (x[i] * x[i]);
  return s;
}

#endif  // USE_HIP

// ===========================================================================
// 对外 7 个接口
// ===========================================================================

// 1. y = A * x
void student_spmv(int n, const int* rowPtr, const int* colIdx,
                  const double* val, const double* x, double* y,
                  student_stream_t stream) {
  if (n <= 0) return;
#ifdef USE_HIP
  const int block = 128;
  int grid = (n + block - 1) / block;
  spmv_csr_kernel<<<grid, block, 0, stream>>>(n, rowPtr, colIdx, val, x, y);
#else
  for (int row = 0; row < n; ++row) {
    double sum = 0.0;
    for (int i = rowPtr[row]; i < rowPtr[row + 1]; ++i)
      sum += val[i] * x[colIdx[i]];
    y[row] = sum;
  }
  (void)stream;
#endif
}

// 2. out = a*x + b*y
void student_axpby(int n, double a, const double* x, double b, const double* y,
                   double* out, student_stream_t stream) {
  if (n <= 0) return;
#ifdef USE_HIP
  const int block = 256;
  int grid = (n + block - 1) / block;
  axpby_kernel<<<grid, block, 0, stream>>>(n, a, x, b, y, out);
#else
  for (int i = 0; i < n; ++i) out[i] = a * x[i] + b * y[i];
  (void)stream;
#endif
}

// 3. out = (alpha*x) .* y
void student_mul(int n, double alpha, const double* x, const double* y,
                 double* out, student_stream_t stream) {
  if (n <= 0) return;
#ifdef USE_HIP
  const int block = 256;
  int grid = (n + block - 1) / block;
  mul_kernel<<<grid, block, 0, stream>>>(n, alpha, x, y, out);
#else
  for (int i = 0; i < n; ++i) out[i] = alpha * x[i] * y[i];
  (void)stream;
#endif
}

// 4. x . y
double student_dot(int n, const double* x, const double* y,
                   student_stream_t stream) {
  if (n <= 0) return 0.0;
  return reduce_launch(n, x, y, true, stream);
}

// 5. ||x||_2
double student_norm2(int n, const double* x, student_stream_t stream) {
  if (n <= 0) return 0.0;
  double sq = reduce_launch(n, x, x, false, stream);
  return std::sqrt(sq);
}

// 6. y = A * x ，A 为 m x n 稠密行主序
void student_gemv(int m, int n, const double* A, const double* x, double* y,
                  student_stream_t stream) {
  if (m <= 0 || n <= 0) return;
#ifdef USE_HIP
  const int block = 128;
  int grid = (m + block - 1) / block;
  gemv_kernel<<<grid, block, 0, stream>>>(m, n, A, x, y);
#else
  for (int row = 0; row < m; ++row) {
    const double* arow = A + (size_t)row * (size_t)n;
    double sum = 0.0;
    for (int j = 0; j < n; ++j) sum += arow[j] * x[j];
    y[row] = sum;
  }
  (void)stream;
#endif
}

// 7. C = A * B ，CSR，输出结构已知
void student_spgemm(int m, int n, int k, const int* aRowPtr, const int* aColIdx,
                    const double* aVal, const int* bRowPtr, const int* bColIdx,
                    const double* bVal, const int* cRowPtr, const int* cColIdx,
                    double* cVal, student_stream_t stream) {
  if (m <= 0) return;
#ifdef USE_HIP
  const int block = 128;
  int grid = (m + block - 1) / block;
  spgemm_kernel<<<grid, block, 0, stream>>>(m, aRowPtr, aColIdx, aVal, bRowPtr,
                                            bColIdx, bVal, cRowPtr, cColIdx,
                                            cVal);
#else
  for (int row = 0; row < m; ++row) {
    int cStart = cRowPtr[row];
    int cEnd = cRowPtr[row + 1];
    int aStart = aRowPtr[row];
    int aEnd = aRowPtr[row + 1];
    for (int ci = cStart; ci < cEnd; ++ci) {
      int j = cColIdx[ci];
      double sum = 0.0;
      for (int ai = aStart; ai < aEnd; ++ai) {
        int kk = aColIdx[ai];
        for (int bi = bRowPtr[kk]; bi < bRowPtr[kk + 1]; ++bi) {
          if (bColIdx[bi] == j) sum += aVal[ai] * bVal[bi];
        }
      }
      cVal[ci] = sum;
    }
  }
  // A 的列数在 CSR 下由 bRowPtr 的跨度决定，k 仅用于对齐接口
  (void)n;
  (void)k;
  (void)stream;
#endif
}
