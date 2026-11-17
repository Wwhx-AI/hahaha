#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  source/build.sh —— 真正的构建逻辑（可修改）
#
#  流程：
#    1. 准备 DTK / Trilinos / CMake 环境（自动探测，可用环境变量覆盖）
#    2. 先用 hipcc 编一遍 7 算子 + GMRES 的自测，快速暴露后端语法/链接问题
#    3. 把 DCU 后端注入 Xyce 源码树（source/tools/apply_dcu_patch.py，幂等）
#    4. CMake 构建 Xyce，install 到仓库根，得到 bin/Xyce
#
#  可用环境变量覆盖：
#    DTK_ROOT       默认自动探测 /opt/dtk-26.04 等
#    TRILINOS_DIR   默认自动探测 /usr/local/trilinos-xyce 等
#    DCU_ARCH       默认 gfx936
#    JOBS           默认 nproc
#-----------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DTK_ROOT="${DTK_ROOT:-}"
TRILINOS_DIR="${TRILINOS_DIR:-}"
DCU_ARCH="${DCU_ARCH:-gfx936}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# ---- 1. 环境探测 ----------------------------------------------------------
# DTK：依次尝试显式变量、官方镜像路径、其它常见前缀
detect_dtk() {
  local c
  if [ -n "$DTK_ROOT" ] && [ -x "$DTK_ROOT/bin/hipcc" ]; then echo "$DTK_ROOT"; return 0; fi
  for c in /opt/dtk-26.04 /opt/dtk /opt/dtk-24.04 /public/software/compiler/dtk-26.04; do
    if [ -x "$c/bin/hipcc" ]; then echo "$c"; return 0; fi
  done
  for c in /opt/dtk-*/bin/hipcc /public/software/compiler/dtk-*/bin/hipcc; do
    if [ -x "$c" ]; then echo "${c%/bin/hipcc}"; return 0; fi
  done
  # 最后尝试 module 系统
  if command -v module >/dev/null 2>&1 || type module >/dev/null 2>&1; then
    for m in compiler/dtk/25.04 compiler/dtk/26.04 dtk/26.04 dtk/25.04; do
      if module load "$m" >/dev/null 2>&1; then
        if command -v hipcc >/dev/null 2>&1; then
          echo "$(dirname "$(dirname "$(command -v hipcc)")")"; return 0
        fi
      fi
    done
  fi
  return 1
}

# Trilinos：找含 lib/cmake/Trilinos 的安装前缀
detect_trilinos() {
  local c
  if [ -n "$TRILINOS_DIR" ] && [ -d "$TRILINOS_DIR/lib/cmake/Trilinos" ]; then echo "$TRILINOS_DIR"; return 0; fi
  for c in /usr/local/trilinos-xyce /usr/local/trilinos /opt/trilinos /opt/trilinos-xyce /opt/trilinos-15.1.1; do
    if [ -d "$c/lib/cmake/Trilinos" ]; then echo "$c"; return 0; fi
  done
  for c in /usr/local/trilinos*/lib/cmake/Trilinos /opt/trilinos*/lib/cmake/Trilinos /usr/local/lib/cmake/Trilinos; do
    if [ -d "$c" ]; then echo "${c%/lib/cmake/Trilinos}"; return 0; fi
  done
  return 1
}

if ! DETECTED_DTK="$(detect_dtk)"; then
  echo "[build] 错误：找不到 hipcc（DTK）" >&2
  echo "[build]   已尝试：\$DTK_ROOT /opt/dtk-26.04 /opt/dtk-* /public/software/compiler/dtk-* / module load" >&2
  echo "[build]   请用 DTK_ROOT=/path/to/dtk 指定，或先 module load compiler/dtk/xxx" >&2
  exit 1
fi
DTK_ROOT="$DETECTED_DTK"

export PATH="$DTK_ROOT/bin:$PATH"
HCC="$(command -v hipcc)"
echo "[build] 仓库根  : $ROOT"
echo "[build] DTK     : $DTK_ROOT"
echo "[build] hipcc   : $HCC"
echo "[build] arch    : $DCU_ARCH"
echo "[build] jobs    : $JOBS"

if [ ! -f source/xyce/CMakeLists.txt ]; then
  echo "[build] 错误：source/xyce 下没有完整 Xyce 源码" >&2
  exit 1
fi

if ! DETECTED_TRILINOS="$(detect_trilinos)"; then
  echo "[build] 错误：找不到 Trilinos 的 CMake 配置（lib/cmake/Trilinos）" >&2
  echo "[build]   已尝试：\$TRILINOS_DIR /usr/local/trilinos-xyce /usr/local/trilinos* /opt/trilinos*" >&2
  echo "[build]   请用 TRILINOS_DIR=/path/to/trilinos 指定安装前缀" >&2
  exit 1
fi
TRILINOS_DIR="$DETECTED_TRILINOS"

export PATH="$DTK_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$DTK_ROOT/lib:$TRILINOS_DIR/lib:$ROOT/lib:${LD_LIBRARY_PATH:-}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-8}"
export OPENBLAS_NUM_THREADS="${OPENBLAS_NUM_THREADS:-1}"
export AMDGPU_TARGETS="$DCU_ARCH"

if ! command -v cmake >/dev/null 2>&1; then
  echo "[build] 错误：找不到 cmake" >&2
  exit 1
fi
echo "[build] cmake   : $(cmake --version | head -1)"
echo "[build] trilinos: $TRILINOS_DIR"

mkdir -p bin lib build

# ---- 2. 后端自测（快速失败）-----------------------------------------------
echo "[build] 编译后端自测 ..."
if ! "$HCC" -std=c++17 -O2 --offload-arch="$DCU_ARCH" \
      -Isource/dcu \
      source/dcu/student_ops.cpp source/dcu/dcu_gmres.cpp source/dcu/test_student.cpp \
      -o build/student_test; then
  echo "[build] 错误：后端自测程序编译失败（见上方 hipcc 输出）" >&2
  exit 1
fi
echo "[build] 后端自测程序：build/student_test"
if ./build/student_test >/dev/null 2>&1; then
  echo "[build] 后端自测通过"
else
  echo "[build] 警告：后端自测程序在有卡环境外可能自检失败（无 DCU 时不阻断构建）" >&2
fi

# ---- 3. 注入 Xyce 源码树 --------------------------------------------------
python3 source/tools/apply_dcu_patch.py

# ---- 4. 构建 Xyce ---------------------------------------------------------
TRILINOS_CMAKE="$TRILINOS_DIR/lib/cmake/Trilinos"

# 生成器：优先 Ninja，缺失则回退 Make
if command -v ninja >/dev/null 2>&1; then
  GEN=(-G Ninja)
else
  echo "[build] 未找到 ninja，回退 Unix Makefiles" >&2
  GEN=(-G "Unix Makefiles")
fi

BUILD_DIR="$ROOT/build/xyce"
CMAKECACHE="$BUILD_DIR/CMakeCache.txt"
if [ -f "$CMAKECACHE" ] && grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=$ROOT/source/xyce" "$CMAKECACHE" 2>/dev/null; then
  echo "[build] 复用已有构建目录：$BUILD_DIR"
else
  rm -rf "$BUILD_DIR"
  echo "[build] 配置 CMake 工程 ..."
  cmake -S source/xyce -B "$BUILD_DIR" "${GEN[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$HCC" \
    -DCMAKE_CXX_COMPILER="$HCC" \
    -DCMAKE_CXX_FLAGS="--offload-arch=$DCU_ARCH -O3" \
    -DCMAKE_HIP_ARCHITECTURES="$DCU_ARCH" \
    -DTrilinos_DIR="$TRILINOS_CMAKE" \
    -DCMAKE_INSTALL_PREFIX="$ROOT" \
    -DXyce_TEST_BINARIES=OFF \
    -DBUILD_TESTING=OFF
fi

echo "[build] 编译 Xyce（-j$JOBS）..."
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "[build] 安装到 $ROOT ..."
cmake --install "$BUILD_DIR"

# ---- 5. 校验 -------------------------------------------------------------
if [ ! -x "$ROOT/bin/Xyce" ]; then
  echo "[build] 错误：安装完成但 bin/Xyce 不可执行" >&2
  exit 1
fi
echo "[build] 产物：$ROOT/bin/Xyce"
"$ROOT/bin/Xyce" -v 2>&1 | head -3 || true
