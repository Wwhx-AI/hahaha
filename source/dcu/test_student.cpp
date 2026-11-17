// 7 个 student_* 接口的本地数值自测。
// 本地 cl 编译运行；DCU 上同样可编译，作为正确性基线。
#include "student_ops.h"
#include "dcu_gmres.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

static bool near_eq(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

static void check(const char* name, bool ok) {
    if (ok) {
        std::printf("[PASS] %s\n", name);
        ++g_pass;
    } else {
        std::printf("[FAIL] %s\n", name);
        ++g_fail;
    }
    return;
}

int main() {
    std::printf("=== student_* 接口自测 ===\n");
    student_stream_t s = nullptr;

    // 1. spmv：A=[2 0 1; 0 3 0; 1 0 4]，x=全1 -> y=[3,3,5]
    {
        std::vector<int> rowPtr = {0, 2, 3, 5};
        std::vector<int> colIdx = {0, 2, 1, 0, 2};
        std::vector<double> val = {2, 1, 3, 1, 4};
        std::vector<double> x = {1, 1, 1};
        std::vector<double> y(3, 0);
        student_spmv(3, rowPtr.data(), colIdx.data(), val.data(), x.data(),
                     y.data(), s);
        check("spmv  y=[3,3,5]",
              near_eq(y[0], 3) && near_eq(y[1], 3) && near_eq(y[2], 5));
    }

    // 2. axpby：out=2x+3y，x=[1,2,3] y=[4,5,6] -> [14,19,24]
    {
        std::vector<double> x = {1, 2, 3};
        std::vector<double> y = {4, 5, 6};
        std::vector<double> out(3, 0);
        student_axpby(3, 2.0, x.data(), 3.0, y.data(), out.data(), s);
        check("axpby out=[14,19,24]",
              near_eq(out[0], 14) && near_eq(out[1], 19) && near_eq(out[2], 24));
    }

    // 3. mul：out=(2x).*y -> [8,20,36]
    {
        std::vector<double> x = {1, 2, 3};
        std::vector<double> y = {4, 5, 6};
        std::vector<double> out(3, 0);
        student_mul(3, 2.0, x.data(), y.data(), out.data(), s);
        check("mul  out=[8,20,36]",
              near_eq(out[0], 8) && near_eq(out[1], 20) && near_eq(out[2], 36));
    }

    // 4. dot：[1,2,3].[4,5,6]=32
    {
        std::vector<double> x = {1, 2, 3};
        std::vector<double> y = {4, 5, 6};
        check("dot  x.y=32", near_eq(student_dot(3, x.data(), y.data(), s), 32));
    }

    // 5. norm2：[3,4] -> 5
    {
        std::vector<double> x = {3, 4};
        check("norm2 ||x||=5",
              near_eq(student_norm2(2, x.data(), s), 5.0));
    }

    // 6. gemv：A=[1 2;3 4]，x=全1 -> [3,7]
    {
        std::vector<double> A = {1, 2, 3, 4};
        std::vector<double> x = {1, 1};
        std::vector<double> y(2, 0);
        student_gemv(2, 2, A.data(), x.data(), y.data(), s);
        check("gemv y=[3,7]", near_eq(y[0], 3) && near_eq(y[1], 7));
    }

    // 7. spgemm：A=B=[1 2;0 3]，C 结构给定 -> cVal=[1,8,0,9]
    {
        std::vector<int> aRow = {0, 2, 3};
        std::vector<int> aCol = {0, 1, 1};
        std::vector<double> aVal = {1, 2, 3};
        std::vector<int> bRow = {0, 2, 3};
        std::vector<int> bCol = {0, 1, 1};
        std::vector<double> bVal = {1, 2, 3};
        std::vector<int> cRow = {0, 2, 4};
        std::vector<int> cCol = {0, 1, 0, 1};
        std::vector<double> cVal(4, 0);
        student_spgemm(2, 2, 2, aRow.data(), aCol.data(), aVal.data(),
                       bRow.data(), bCol.data(), bVal.data(), cRow.data(),
                       cCol.data(), cVal.data(), s);
        check("spgemm C=[1,8,0,9]",
              near_eq(cVal[0], 1) && near_eq(cVal[1], 8) &&
              near_eq(cVal[2], 0) && near_eq(cVal[3], 9));
    }

    // 8. DCU GMRES：求解 3x3 稀疏系统，回代校验 A x 与 b 一致
    {
        const int n = 3;
        std::vector<int> rowPtr = {0, 2, 5, 7};
        std::vector<int> colIdx = {0, 1, 0, 1, 2, 1, 2};
        std::vector<double> vals = {4, 1, 1, 3, 1, 1, 2};
        std::vector<double> b = {1, 2, 3};
        std::vector<double> x(n, 0.0);

        int iters = dcu_gmres_solve(n, rowPtr.data(), colIdx.data(),
                                    vals.data(), b.data(), x.data(), 30, 200,
                                    1e-12);

        std::vector<double> ax(n, 0.0);
        student_spmv(n, rowPtr.data(), colIdx.data(), vals.data(), x.data(),
                     ax.data(), s);
        double err = 0.0;
        for (int i = 0; i < n; ++i)
            err = std::fmax(err, std::fabs(ax[i] - b[i]));
        check("gmres  Ax=b", iters >= 0 && err < 1e-8);
    }

    std::printf("-----------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0) {
        std::printf("结果: ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("结果: TESTS FAILED\n");
    return 1;
}
