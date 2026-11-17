//-----------------------------------------------------------------------------
//  dcu_gmres.h
//
//  DCU 上的右预条件 GMRES(m) 求解器（Jacobi 预条件）。
//  向量层运算全部落在 student_ops 的 7 个接口上，便于统一替换/优化。
//
//  与 Xyce 的 N_LAS_DCUSolver 解耦：这里只面对裸 CSR 与裸数组，
//  由适配层负责把 Xyce 的 Matrix / MultiVector 内容搬进搬出。
//-----------------------------------------------------------------------------
#ifndef DCU_GMRES_H
#define DCU_GMRES_H

#ifdef __cplusplus
extern "C" {
#endif

// 求解 A x = b
//   n       : 矩阵阶数（CSR 行数）
//   rowPtr  : 长度 n+1
//   colIdx  : 长度 nnz
//   vals    : 长度 nnz
//   b       : 长度 n，右端项
//   x       : 长度 n，输入为初值，输出为解
//   restart : 重启维数 m（建议 30；<=0 时取 min(n,30)）
//   max_iter: 最大总迭代数（建议 500）
//   tol     : 相对残差收敛阈值（相对 ||b||）
// 返回值：实际迭代次数（>=0）；负数表示失败
//   -1 : 参数非法
//   -2 : 设备内存不足
//   -3 : Arnoldi 崩溃且残差未达标
int dcu_gmres_solve(int n, const int* rowPtr, const int* colIdx,
                    const double* vals, const double* b, double* x,
                    int restart, int max_iter, double tol);

#ifdef __cplusplus
}
#endif

#endif  // DCU_GMRES_H
