#pragma once

// 组委会 DCU 可替换后端的 7 个基础运算接口（学生实现）。
// 接口统一使用 double 数组、int 索引、hipStream_t 流。
// 说明：本骨架签名为可编译参考，正式提交以组委会 source/dcu 实际头文件为准。

#include <cstddef>

// 自动判定：用 hipcc 编译时不需要额外加 -DUSE_HIP
#if !defined(USE_HIP) && defined(__HIPCC__)
#define USE_HIP 1
#endif

#ifdef USE_HIP
#include <hip/hip_runtime.h>
using student_stream_t = hipStream_t;
#else
// CPU 参考 / 本地自测：流用 void* 占位，不参与计算
using student_stream_t = void*;
#endif

extern "C" {

// 1. CSR 稀疏矩阵向量乘 y = A*x
//    n: 行数；rowPtr 长度 n+1；colIdx/val 长度 nnz
void student_spmv(int n, const int* rowPtr, const int* colIdx,
                  const double* val, const double* x, double* y,
                  student_stream_t stream);

// 2. 向量线性组合 out = a*x + b*y
void student_axpby(int n, double a, const double* x,
                   double b, const double* y, double* out,
                   student_stream_t stream);

// 3. 逐元素乘法 out = (alpha*x) .* y
void student_mul(int n, double alpha, const double* x,
                 const double* y, double* out,
                 student_stream_t stream);

// 4. 内积 x^T y
double student_dot(int n, const double* x, const double* y,
                   student_stream_t stream);

// 5. 二范数 ||x||_2
double student_norm2(int n, const double* x, student_stream_t stream);

// 6. 稠密矩阵向量乘 y = A*x（A 行主序，m 行 n 列）
void student_gemv(int m, int n, const double* A, const double* x,
                  double* y, student_stream_t stream);

// 7. 已知输出结构的稀疏矩阵乘积数值计算 C = A*B（CSR）
//    A: m x k，B: k x n，C: m x n；C 的结构已给定，仅填 cVal
void student_spgemm(int m, int n, int k,
                    const int* aRowPtr, const int* aColIdx, const double* aVal,
                    const int* bRowPtr, const int* bColIdx, const double* bVal,
                    const int* cRowPtr, const int* cColIdx, double* cVal,
                    student_stream_t stream);

}  // extern "C"
