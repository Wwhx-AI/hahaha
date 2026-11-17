#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  run.sh —— 本地编译与自测工具（不算评测入口）
#
#  用法：
#    bash run.sh                     只跑 7 算子 + GMRES 自测
#    bash run.sh <电路.cir>          用 bin/Xyce 以 DCUGMRES 求解该电路
#    bash run.sh <电路.cir> KLU      指定其他求解器
#-----------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

DTK_ROOT="${DTK_ROOT:-/opt/dtk-26.04}"
TRILINOS_DIR="${TRILINOS_DIR:-/usr/local/trilinos-xyce}"
DCU_ARCH="${DCU_ARCH:-gfx936}"

export PATH="$DTK_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$DTK_ROOT/lib:$TRILINOS_DIR/lib:$ROOT/lib:${LD_LIBRARY_PATH:-}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-8}"
export OPENBLAS_NUM_THREADS="${OPENBLAS_NUM_THREADS:-1}"
export AMDGPU_TARGETS="$DCU_ARCH"

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
  if command -v hipcc >/dev/null 2>&1; then
    CXX=hipcc
  else
    CXX=g++
  fi
fi

mkdir -p build

echo "[run] 编译器：$CXX"
FLAGS="-std=c++17 -O2 -Isource/dcu"
if [ "$CXX" = "hipcc" ]; then
  FLAGS="$FLAGS --offload-arch=$DCU_ARCH"
fi

"$CXX" $FLAGS \
  source/dcu/student_ops.cpp source/dcu/dcu_gmres.cpp source/dcu/test_student.cpp \
  -o build/student_test

./build/student_test

CIR="${1:-}"
if [ -n "$CIR" ]; then
  if [ ! -x "$ROOT/bin/Xyce" ]; then
    echo "[run] 没有 bin/Xyce，先执行：bash build.sh" >&2
    exit 1
  fi
  SOLVER="${2:-DCUGMRES}"
  echo "[run] $ROOT/bin/Xyce -linsolv $SOLVER $CIR"
  "$ROOT/bin/Xyce" -linsolv "$SOLVER" "$CIR"
fi
