//-----------------------------------------------------------------------------
//  dcu_gmres.cpp
//
//  右预条件 GMRES(m)，预条件为 Jacobi（A 的对角线倒数）。
//  内层 Krylov 的全部向量运算都通过 student_* 接口完成，
//  即：SpMV -> student_spmv，内积 -> student_dot，范数 -> student_norm2，
//      axpby 更新 -> student_axpby，Jacobi 预条件 -> student_mul。
//-----------------------------------------------------------------------------
#include "dcu_gmres.h"
#include "student_ops.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef USE_HIP
#include <hip/hip_runtime.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// 设备侧内存封装：USE_HIP 时走 hip*，否则退化为同一台机器上的 malloc
// ---------------------------------------------------------------------------
#ifdef USE_HIP
void* dev_alloc(size_t bytes) {
  void* p = 0;
  if (hipMalloc(&p, bytes) != hipSuccess) return 0;
  return p;
}
void dev_free(void* p) {
  if (p) hipFree(p);
}
void dev_h2d(void* d, const void* h, size_t bytes) {
  hipMemcpy(d, h, bytes, hipMemcpyHostToDevice);
}
void dev_d2h(void* h, const void* d, size_t bytes) {
  hipMemcpy(h, d, bytes, hipMemcpyDeviceToHost);
}
void dev_zero(void* d, size_t bytes) { hipMemset(d, 0, bytes); }
#else
void* dev_alloc(size_t bytes) { return malloc(bytes); }
void dev_free(void* p) { free(p); }
void dev_h2d(void* d, const void* h, size_t bytes) { memcpy(d, h, bytes); }
void dev_d2h(void* h, const void* d, size_t bytes) { memcpy(h, d, bytes); }
void dev_zero(void* d, size_t bytes) { memset(d, 0, bytes); }
#endif

}  // namespace

extern "C" int dcu_gmres_solve(int n, const int* rowPtr, const int* colIdx,
                               const double* vals, const double* b, double* x,
                               int restart, int max_iter, double tol) {
  if (n <= 0 || !rowPtr || !colIdx || !vals || !b || !x) return -1;

  if (restart <= 0) restart = (n < 30) ? n : 30;
  if (max_iter <= 0) max_iter = 500;
  if (tol <= 0.0) tol = 1e-10;

  int m = restart;
  int nnz = rowPtr[n];  // CSR 定义：非零元总数为 rowPtr[n]

  // ---- 设备缓冲 -----------------------------------------------------------
  int* dRow = (int*)dev_alloc((size_t)(n + 1) * sizeof(int));
  int* dCol = (int*)dev_alloc((size_t)(nnz > 0 ? nnz : 1) * sizeof(int));
  double* dVal = (double*)dev_alloc((size_t)(nnz > 0 ? nnz : 1) * sizeof(double));
  double* dInvDiag = (double*)dev_alloc((size_t)n * sizeof(double));
  double* dB = (double*)dev_alloc((size_t)n * sizeof(double));
  double* dX = (double*)dev_alloc((size_t)n * sizeof(double));
  double* dR = (double*)dev_alloc((size_t)n * sizeof(double));
  double* dW = (double*)dev_alloc((size_t)n * sizeof(double));
  double* dZ = (double*)dev_alloc((size_t)m * (size_t)n * sizeof(double));
  double* dV = (double*)dev_alloc((size_t)(m + 1) * (size_t)n * sizeof(double));

  if (!dRow || !dCol || !dVal || !dInvDiag || !dB || !dX || !dR || !dW || !dZ ||
      !dV) {
    dev_free(dRow); dev_free(dCol); dev_free(dVal); dev_free(dInvDiag);
    dev_free(dB); dev_free(dX); dev_free(dR); dev_free(dW);
    dev_free(dZ); dev_free(dV);
    return -2;
  }

  const student_stream_t st = 0;

  // ---- 上传矩阵结构与初值 -------------------------------------------------
  dev_h2d(dRow, rowPtr, (size_t)(n + 1) * sizeof(int));
  if (nnz > 0) {
    dev_h2d(dCol, colIdx, (size_t)nnz * sizeof(int));
    dev_h2d(dVal, vals, (size_t)nnz * sizeof(double));
  }
  dev_h2d(dB, b, (size_t)n * sizeof(double));
  dev_h2d(dX, x, (size_t)n * sizeof(double));

  // ---- Jacobi 预条件：取对角线倒数 ---------------------------------------
  {
    std::vector<double> invDiag((size_t)n, 1.0);
    for (int i = 0; i < n; ++i) {
      double d = 0.0;
      for (int p = rowPtr[i]; p < rowPtr[i + 1]; ++p) {
        if (colIdx[p] == i) { d = vals[p]; break; }
      }
      invDiag[(size_t)i] = (d != 0.0) ? (1.0 / d) : 1.0;
    }
    dev_h2d(dInvDiag, &invDiag[0], (size_t)n * sizeof(double));
  }

  // ---- 常量与工作区（主机侧） ---------------------------------------------
  std::vector<double> h((size_t)(m + 1) * (size_t)m, 0.0);
  std::vector<double> cs((size_t)m, 0.0), sn((size_t)m, 0.0);
  std::vector<double> g((size_t)(m + 1), 0.0);
  std::vector<double> ysol((size_t)m, 0.0);

  const double bnorm = student_norm2(n, dB, st);
  if (bnorm == 0.0) {
    dev_zero(dX, (size_t)n * sizeof(double));
    dev_d2h(x, dX, (size_t)n * sizeof(double));
    dev_free(dRow); dev_free(dCol); dev_free(dVal); dev_free(dInvDiag);
    dev_free(dB); dev_free(dX); dev_free(dR); dev_free(dW);
    dev_free(dZ); dev_free(dV);
    return 0;
  }

  int total_iters = 0;
  int rc = 0;
  double resid = 0.0;

  while (total_iters < max_iter) {
    // r = b - A x
    student_spmv(n, dRow, dCol, dVal, dX, dW, st);
    student_axpby(n, 1.0, dB, -1.0, dW, dR, st);

    double beta = student_norm2(n, dR, st);
    if (beta <= tol * bnorm) break;

    // V[0] = r / beta
    student_axpby(n, 1.0 / beta, dR, 0.0, dR, dV, st);

    std::fill(h.begin(), h.end(), 0.0);
    std::fill(cs.begin(), cs.end(), 0.0);
    std::fill(sn.begin(), sn.end(), 0.0);
    std::fill(g.begin(), g.end(), 0.0);
    std::fill(ysol.begin(), ysol.end(), 0.0);
    g[0] = beta;

    int k = 0;  // 本轮实际完成的 Arnoldi 步数
    for (int j = 0; j < m; ++j) {
      double* Vj = dV + (size_t)j * (size_t)n;
      double* Zj = dZ + (size_t)j * (size_t)n;

      // z = M^{-1} V[j]
      student_mul(n, 1.0, dInvDiag, Vj, Zj, st);
      // w = A z
      student_spmv(n, dRow, dCol, dVal, Zj, dW, st);

      // 修正 Gram-Schmidt
      for (int i = 0; i <= j; ++i) {
        double* Vi = dV + (size_t)i * (size_t)n;
        double hij = student_dot(n, dW, Vi, st);
        h[(size_t)i * (size_t)m + (size_t)j] = hij;
        student_axpby(n, 1.0, dW, -hij, Vi, dW, st);
      }

      double hnext = student_norm2(n, dW, st);
      h[(size_t)(j + 1) * (size_t)m + (size_t)j] = hnext;

      bool breakdown = (hnext <= 1e-14 * (beta > 0.0 ? beta : 1.0));
      if (!breakdown) {
        double* Vnext = dV + (size_t)(j + 1) * (size_t)n;
        student_axpby(n, 1.0 / hnext, dW, 0.0, dW, Vnext, st);
      }

      // 应用已有 Givens 旋转
      for (int i = 0; i < j; ++i) {
        double t = cs[i] * h[(size_t)i * (size_t)m + (size_t)j] +
                   sn[i] * h[(size_t)(i + 1) * (size_t)m + (size_t)j];
        h[(size_t)(i + 1) * (size_t)m + (size_t)j] =
            -sn[i] * h[(size_t)i * (size_t)m + (size_t)j] +
            cs[i] * h[(size_t)(i + 1) * (size_t)m + (size_t)j];
        h[(size_t)i * (size_t)m + (size_t)j] = t;
      }

      // 生成新的 Givens 旋转消去 h[j+1][j]
      double a = h[(size_t)j * (size_t)m + (size_t)j];
      double bb = h[(size_t)(j + 1) * (size_t)m + (size_t)j];
      double r = std::sqrt(a * a + bb * bb);
      if (r == 0.0) {
        cs[j] = 1.0;
        sn[j] = 0.0;
      } else {
        cs[j] = a / r;
        sn[j] = bb / r;
      }
      h[(size_t)j * (size_t)m + (size_t)j] = r;
      h[(size_t)(j + 1) * (size_t)m + (size_t)j] = 0.0;

      g[j + 1] = -sn[j] * g[j];
      g[j] = cs[j] * g[j];

      resid = std::fabs(g[j + 1]);
      ++total_iters;
      ++k;

      if (breakdown || resid <= tol * bnorm) break;
    }

    if (k == 0) { rc = -3; break; }

    // ---- 回代求 y -----------------------------------------------------------
    for (int i = k - 1; i >= 0; --i) {
      double s = g[i];
      for (int j = i + 1; j < k; ++j)
        s -= h[(size_t)i * (size_t)m + (size_t)j] * ysol[(size_t)j];
      double diag = h[(size_t)i * (size_t)m + (size_t)i];
      ysol[(size_t)i] = (diag != 0.0) ? (s / diag) : 0.0;
    }

    // ---- x += Z * y ---------------------------------------------------------
    dev_zero(dW, (size_t)n * sizeof(double));
    for (int i = 0; i < k; ++i) {
      double* Zi = dZ + (size_t)i * (size_t)n;
      student_axpby(n, ysol[(size_t)i], Zi, 1.0, dW, dW, st);
    }
    student_axpby(n, 1.0, dX, 1.0, dW, dX, st);

    if (resid <= tol * bnorm) break;
  }

  if (resid > tol * bnorm) rc = (rc != 0) ? rc : -3;

  dev_d2h(x, dX, (size_t)n * sizeof(double));

  dev_free(dRow); dev_free(dCol); dev_free(dVal); dev_free(dInvDiag);
  dev_free(dB); dev_free(dX); dev_free(dR); dev_free(dW);
  dev_free(dZ); dev_free(dV);

  return (rc == 0) ? total_iters : rc;
}
