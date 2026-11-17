//-----------------------------------------------------------------------------
//  N_LAS_DCUSolver.C
//
//  Xyce 线性求解器的 DCU 实现：
//    1. 从 Xyce::Linear::Matrix 逐行取零拷贝行视图，拼成扁平 CSR；
//    2. 从 RHS 取右端项，以 0 为初值；
//    3. 调用 dcu_gmres_solve 在 DCU 上做右预条件 GMRES(m)；
//    4. 把解写回 LHS。
//-----------------------------------------------------------------------------
#include <Xyce_config.h>

// ---------- Xyce includes ----------
#include <N_LAS_DCUSolver.h>

#include <N_LAS_Problem.h>
#include <N_LAS_Matrix.h>
#include <N_LAS_MultiVector.h>
#include <N_LAS_Vector.h>
#include <N_LAS_Graph.h>

#include <N_UTL_fwd.h>
#include <N_UTL_Timer.h>
#include <N_UTL_Param.h>
#include <N_UTL_FeatureTest.h>
#include <N_UTL_OptionBlock.h>

#include <N_ERH_ErrorMgr.h>

// ---------- DCU backend ----------
#include <dcu_gmres.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace Xyce {
namespace Linear {

//-----------------------------------------------------------------------------
// Function      : DCUSolver::DCUSolver
//-----------------------------------------------------------------------------
DCUSolver::DCUSolver(Problem& prob, Util::OptionBlock& options)
    : Solver(prob, true),
      options_(new Util::OptionBlock(options)),
      tolerance_(1.0e-10),
      residual_(0.0),
      maxIter_(500),
      restart_(30) {
  setOptions(options);
}

//-----------------------------------------------------------------------------
// Function      : DCUSolver::~DCUSolver
//-----------------------------------------------------------------------------
DCUSolver::~DCUSolver() { delete options_; }

//-----------------------------------------------------------------------------
// Function      : DCUSolver::setOptions
//-----------------------------------------------------------------------------
bool DCUSolver::setOptions(const Util::OptionBlock& OB) {
  if (options_) delete options_;
  options_ = new Util::OptionBlock(OB);
  return true;
}

//-----------------------------------------------------------------------------
// Function      : DCUSolver::setParam
// Purpose       : 允许 .options / .TRAN 里对求解器细调
//-----------------------------------------------------------------------------
bool DCUSolver::setParam(const Util::Param& param) {
  std::string tag = param.uTag();
  std::string sv = param.usVal();
  if (sv.empty()) return true;

  if (tag == "TOL" || tag == "TOLERANCE") {
    double v = atof(sv.c_str());
    if (v > 0.0) tolerance_ = v;
  } else if (tag == "MAXITER" || tag == "OUTPUT") {
    int v = atoi(sv.c_str());
    if (v > 0) maxIter_ = v;
  } else if (tag == "RESTART" || tag == "KSPACE") {
    int v = atoi(sv.c_str());
    if (v > 0) restart_ = v;
  }
  return true;
}

//-----------------------------------------------------------------------------
// Function      : DCUSolver::getInfo
//-----------------------------------------------------------------------------
bool DCUSolver::getInfo(Util::Param& info) {
  (void)info;
  return true;
}

//-----------------------------------------------------------------------------
// Function      : DCUSolver::buildCSR
// Purpose       : 把 Xyce 矩阵转成扁平 CSR；transpose 时转成 A^T
//-----------------------------------------------------------------------------
void DCUSolver::buildCSR(int n, bool useTranspose, std::vector<int>& rowPtr,
                         std::vector<int>& colIdx, std::vector<double>& vals) {
  Matrix* A = lasProblem_.getMatrix();

  rowPtr.assign((size_t)n + 1, 0);
  colIdx.clear();
  vals.clear();
  colIdx.reserve((size_t)n * 7);
  vals.reserve((size_t)n * 7);

  // Xyce/Epetra 的行视图指向内部存储，可直接按行拼接，无需逐元素拷贝
  for (int i = 0; i < n; ++i) {
    int len = 0;
    double* v = 0;
    int* idx = 0;
    A->getLocalRowView(i, len, v, idx);

    for (int p = 0; p < len; ++p) {
      colIdx.push_back(idx[p]);
      vals.push_back(v[p]);
    }
    rowPtr[(size_t)i + 1] = (int)colIdx.size();
  }

  if (!useTranspose) return;

  // ---- 转置：A^T 的 CSR ------------------------------------------------
  std::vector<int> tRowPtr((size_t)n + 1, 0);
  std::vector<int> tColIdx(colIdx.size(), 0);
  std::vector<double> tVals(vals.size(), 0.0);

  // 统计 A^T 每行的非零数（= A 每列的非零数）
  for (size_t p = 0; p < colIdx.size(); ++p) tRowPtr[(size_t)colIdx[p] + 1] += 1;
  for (int i = 0; i < n; ++i) tRowPtr[(size_t)i + 1] += tRowPtr[(size_t)i];

  std::vector<int> cursor(tRowPtr.begin(), tRowPtr.end() - 1);
  for (int i = 0; i < n; ++i) {
    for (int p = rowPtr[(size_t)i]; p < rowPtr[(size_t)i + 1]; ++p) {
      int c = colIdx[(size_t)p];
      int pos = cursor[(size_t)c]++;
      tColIdx[(size_t)pos] = i;
      tVals[(size_t)pos] = vals[(size_t)p];
    }
  }

  rowPtr.swap(tRowPtr);
  colIdx.swap(tColIdx);
  vals.swap(tVals);
}

//-----------------------------------------------------------------------------
// Function      : DCUSolver::doSolve
// Purpose       : x = A^(-1) b
//-----------------------------------------------------------------------------
int DCUSolver::doSolve(bool reuse_factors, bool transpose) {
  (void)reuse_factors;

  Util::Timer timer;
  timer.resetStartTime();

  Matrix* A = lasProblem_.getMatrix();
  if (!A) {
    Report::UserError0() << "DCUGMRES: linear problem has no matrix";
    return 1;
  }

  int n = A->getNumRows();
  if (n <= 0) return 0;

  std::vector<int> rowPtr;
  std::vector<int> colIdx;
  std::vector<double> vals;
  buildCSR(n, transpose, rowPtr, colIdx, vals);

  MultiVector* rhs = lasProblem_.getRHS();
  MultiVector* lhs = lasProblem_.getLHS();
  if (!rhs || !lhs) return 1;

  std::vector<double> b((size_t)n, 0.0);
  std::vector<double> x((size_t)n, 0.0);
  for (int i = 0; i < n; ++i) {
    double* p = (*rhs)(i, 0);
    b[(size_t)i] = p ? p[0] : 0.0;
  }

  int iters = dcu_gmres_solve(n, &rowPtr[0], &colIdx[0], &vals[0], &b[0], &x[0],
                              restart_, maxIter_, tolerance_);

  if (iters < 0) {
    Report::UserWarning0() << "DCUGMRES: GMRES failed to converge (code "
                           << iters << "), returning current iterate";
  }

  for (int i = 0; i < n; ++i) {
    double* p = (*lhs)(i, 0);
    if (p) p[0] = x[(size_t)i];
  }

  // ---- 残差（相对 ||b||）------------------------------------------------
  double bnorm = 0.0;
  for (int i = 0; i < n; ++i) bnorm += b[(size_t)i] * b[(size_t)i];
  bnorm = std::sqrt(bnorm);

  std::vector<double> ax((size_t)n, 0.0);
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int p = rowPtr[(size_t)i]; p < rowPtr[(size_t)i + 1]; ++p)
      s += vals[(size_t)p] * x[(size_t)colIdx[(size_t)p]];
    ax[(size_t)i] = s - b[(size_t)i];
  }
  double rnorm = 0.0;
  for (int i = 0; i < n; ++i) rnorm += ax[(size_t)i] * ax[(size_t)i];
  rnorm = std::sqrt(rnorm);
  residual_ = (bnorm > 0.0) ? (rnorm / bnorm) : rnorm;

  if (VERBOSE_LINEAR) {
    Xyce::lout() << "  DCUGMRES Solve: iters=" << iters
                 << " relres=" << residual_ << std::endl;
  }

  solutionTime_ = timer.elapsedTime();
  return 0;
}

}  // namespace Linear
}  // namespace Xyce
