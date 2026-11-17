//-----------------------------------------------------------------------------
//  N_LAS_DCUSolver.h
//
//  Xyce 线性求解器在 DCU 上的适配层。
//  继承 Xyce::Linear::Solver，把 Xyce 的 Matrix/MultiVector 内容取成 CSR
//  与裸数组，交给 dcu_gmres_solve 在 DCU 上求解，再把结果写回 LHS。
//
//  通过命令行 "-linsolv DCUGMRES" 选用。
//-----------------------------------------------------------------------------
#ifndef Xyce_N_LAS_DCUSolver_h
#define Xyce_N_LAS_DCUSolver_h

#include <N_LAS_Solver.h>
#include <N_LAS_fwd.h>
#include <N_UTL_fwd.h>

#include <vector>

namespace Xyce {
namespace Linear {

//-----------------------------------------------------------------------------
// Class         : DCUSolver
// Purpose       : DCU 后端 GMRES 求解器
//------------------------------------------------------------------------------
class DCUSolver : public Solver {
public:
  DCUSolver(Problem& problem, Util::OptionBlock& options);

  ~DCUSolver();

  bool setOptions(const Util::OptionBlock& OB);
  bool setDefaultOptions() { return true; }
  bool setPreconditioner(const Teuchos::RCP<Preconditioner>& precond) {
    return true;
  }
  bool setNewtonIter(int nIter) { return true; }
  bool setParam(const Util::Param& param);

  bool getInfo(Util::Param& info);
  double residual() { return residual_; }
  void setTolerance(const double& tol) { tolerance_ = tol; }

  // x = A^(-1) b
  int doSolve(bool reuse_factors, bool transpose = false);

private:
  void buildCSR(int n, bool useTranspose, std::vector<int>& rowPtr,
                std::vector<int>& colIdx, std::vector<double>& vals);

  Util::OptionBlock* options_;
  double tolerance_;
  double residual_;
  int maxIter_;
  int restart_;
};
}  // namespace Linear
}  // namespace Xyce

#endif  // Xyce_N_LAS_DCUSolver_h
