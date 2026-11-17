#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  source/build.sh —— 真正的构建逻辑（可修改）
#
#  顺序原则：先把环境探明白、尽早失败，再动手编译。
#    1. 探测 DTK(hipcc) / Trilinos / CMake，任何一项缺失立刻带诊断退出
#    2. 策略 A：hipcc 编译（DCU 加速版，注入 DCUGMRES 后端）
#    3. 策略 B：g++ 编译（纯 CPU 保底版，不注入后端）
#       仅在 A 配置/编译失败时启用，保证产物 bin/Xyce 一定存在
#
#  可用环境变量覆盖：
#    DTK_ROOT / TRILINOS_DIR / TRILINOS_ROOT / DCU_ARCH / JOBS / FORCE_CPU
#-----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

DCU_ARCH="${DCU_ARCH:-gfx936}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
FORCE_CPU="${FORCE_CPU:-0}"

say() { echo "[build] $*"; }
warn() { echo "[build] 警告：$*" >&2; }
die() { echo "[build] 错误：$*" >&2; exit 1; }

# ---- 1. 环境探测 ----------------------------------------------------------
# 从 cmake 配置目录反推安装前缀：<prefix>/lib/cmake/Trilinos/xxx.cmake
prefix_from_cmake() {
  local f="$1" d
  d="$(dirname "$f")"; d="$(dirname "$d")"; d="$(dirname "$d")"; d="$(dirname "$d")"
  echo "$d"
}

# 在给定前缀下找 Trilinos 的 cmake 配置目录，成功则打印该目录
# 兼容两种布局：<prefix>/lib/cmake/Trilinos/... 与 <prefix>/include/TrilinosConfig.cmake
tril_cmake_in() {
  local p="${1:-}" sub
  [ -n "$p" ] || return 1
  if [ -f "$p/TrilinosConfig.cmake" ]; then echo "$p"; return 0; fi
  for sub in include lib/cmake/Trilinos lib64/cmake/Trilinos share/trilinos/cmake share/cmake/Trilinos; do
    if [ -f "$p/$sub/TrilinosConfig.cmake" ]; then echo "$p/$sub"; return 0; fi
  done
  return 1
}

detect_dtk() {
  local c
  if [ -n "${DTK_ROOT:-}" ] && [ -x "$DTK_ROOT/bin/hipcc" ]; then echo "$DTK_ROOT"; return 0; fi
  for c in /opt/dtk-26.04 /opt/dtk /opt/dtk-24.04 /public/software/compiler/dtk-26.04; do
    if [ -x "$c/bin/hipcc" ]; then echo "$c"; return 0; fi
  done
  for c in /opt/dtk-*/bin/hipcc /public/software/compiler/dtk-*/bin/hipcc /usr/local/dtk*/bin/hipcc; do
    if [ -x "$c" ]; then echo "${c%/bin/hipcc}"; return 0; fi
  done
  # module 系统兜底
  if type module >/dev/null 2>&1; then
    for m in compiler/dtk/26.04 compiler/dtk/25.04 dtk/26.04 dtk/25.04; do
      if module load "$m" >/dev/null 2>&1 && command -v hipcc >/dev/null 2>&1; then
        echo "$(dirname "$(dirname "$(command -v hipcc)")")"; return 0
      fi
    done
  fi
  return 1
}

TRILINOS_CMAKE=""
TRILINOS_PREFIX=""

detect_trilinos() {
  local p c f d

  # 1) 显式环境变量（含 CMake 风格的 Trilinos_DIR）
  for p in "${Trilinos_DIR:-}" "${TRILINOS_DIR:-}" "${TRILINOS_ROOT:-}" "${TRILINOS_HOME:-}" "${TRILINOS_PREFIX:-}" "${Trilinos_ROOT:-}"; do
    [ -n "$p" ] || continue
    if c="$(tril_cmake_in "$p")"; then TRILINOS_CMAKE="$c"; TRILINOS_PREFIX="$p"; return 0; fi
  done

  # 2) 常见固定前缀
  for p in /usr/local/trilinos-xyce /usr/local/trilinos /opt/trilinos /opt/trilinos-xyce \
           /opt/trilinos-15.1.1 /opt/trilinos-15.1 /usr/local/trilinos-15.1.1 \
           /usr/local/trilinos-15.1 /usr/trilinos /usr/local/Trilinos /opt/Trilinos; do
    if c="$(tril_cmake_in "$p")"; then TRILINOS_CMAKE="$c"; TRILINOS_PREFIX="$p"; return 0; fi
  done

  # 3) glob 扫描
  for p in /opt/trilinos* /usr/local/trilinos* /usr/local/Trilinos* /usr/trilinos* \
           /public/software/*trilinos* /public/software/mathlib/*trilinos*; do
    [ -d "$p" ] || continue
    if c="$(tril_cmake_in "$p")"; then TRILINOS_CMAKE="$c"; TRILINOS_PREFIX="$p"; return 0; fi
  done

  # 4) module 系统兜底
  if type module >/dev/null 2>&1; then
    for m in trilinos trilinos/15.1.1 trilinos-xyce trilinos/15.1; do
      if module load "$m" >/dev/null 2>&1; then
        for p in "${TRILINOS_DIR:-}" "${TRILINOS_ROOT:-}"; do
          [ -n "$p" ] || continue
          if c="$(tril_cmake_in "$p")"; then TRILINOS_CMAKE="$c"; TRILINOS_PREFIX="$p"; return 0; fi
        done
      fi
    done
  fi

  # 5) find 兜底（限目录、限深度）
  for d in /usr /usr/local /opt /public /home /data; do
    [ -d "$d" ] || continue
    f="$(find "$d" -maxdepth 8 -name 'TrilinosConfig.cmake' -type f 2>/dev/null | head -1)"
    if [ -n "$f" ]; then
      TRILINOS_CMAKE="$(dirname "$f")"
      TRILINOS_PREFIX="$(prefix_from_cmake "$f")"
      return 0
    fi
  done
  return 1
}

# ---- 2. 环境报告 ----------------------------------------------------------
say "仓库根  : $ROOT"
say "arch    : $DCU_ARCH"
say "jobs    : $JOBS"

if ! DETECTED_DTK="$(detect_dtk)"; then
  die "找不到 hipcc（DTK）。已尝试 \$DTK_ROOT /opt/dtk* /public/software/compiler/dtk* / module load"
fi
DTK_ROOT="$DETECTED_DTK"
export PATH="$DTK_ROOT/bin:$PATH"
HCC="$(command -v hipcc)"
say "DTK     : $DTK_ROOT"
say "hipcc   : $HCC"

if ! command -v cmake >/dev/null 2>&1; then die "找不到 cmake"; fi
say "cmake   : $(cmake --version 2>/dev/null | head -1)"

if ! detect_trilinos; then
  echo "[build] 已探测 Trilinos 前缀（均未命中）：" >&2
  echo "[build]   \$TRILINOS_DIR \$TRILINOS_ROOT \${TRILINOS_HOME}" >&2
  echo "[build]   /usr/local/trilinos-xyce /usr/local/trilinos /opt/trilinos* /usr/trilinos" >&2
  echo "[build]   module load trilinos；以及 find /usr /opt /public -name TrilinosConfig.cmake" >&2
  echo "[build] 现存候选目录：" >&2
  ls -d /opt/*trilinos* /usr/local/*rilinos* 2>/dev/null | head -20 >&2 || true
  die "找不到 Trilinos 的 CMake 配置（TrilinosConfig.cmake）"
fi
say "trilinos: $TRILINOS_PREFIX"
say "TriCMake: $TRILINOS_CMAKE"

export LD_LIBRARY_PATH="$DTK_ROOT/lib:$DTK_ROOT/lib64:$TRILINOS_PREFIX/lib:$TRILINOS_PREFIX/lib64:$(dirname "$TRILINOS_PREFIX")/lib:$(dirname "$TRILINOS_PREFIX")/lib64:$ROOT/lib:${LD_LIBRARY_PATH:-}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-8}"
export OPENBLAS_NUM_THREADS="${OPENBLAS_NUM_THREADS:-1}"
export AMDGPU_TARGETS="$DCU_ARCH"

if [ ! -f source/xyce/CMakeLists.txt ]; then die "source/xyce 下没有完整 Xyce 源码"; fi

mkdir -p bin lib build

# 生成器：优先 Ninja，缺失则回退 Make
if command -v ninja >/dev/null 2>&1; then
  GEN=(-G Ninja)
else
  warn "未找到 ninja，回退 Unix Makefiles"
  GEN=(-G "Unix Makefiles")
fi

# ---- 3. 构建函数 ----------------------------------------------------------
# configure_xyce <build_dir> <cc> <cxx> [额外 cmake 参数...]
configure_xyce() {
  local build_dir="$1" cc="$2" cxx="$3"; shift 3
  rm -rf "$build_dir"
  cmake -S source/xyce -B "$build_dir" "${GEN[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_CXX_COMPILER="$cxx" \
    "$@" \
    -DTrilinos_DIR="$TRILINOS_CMAKE" \
    -DCMAKE_INSTALL_PREFIX="$ROOT" \
    -DXyce_TEST_BINARIES=OFF \
    -DBUILD_TESTING=OFF
}

# ---- 4. 策略 A：hipcc / DCU 加速版 ---------------------------------------
try_dcu_build() {
  say "策略 A：hipcc 编译（DCU 加速版）"

  # 后端自测编译：只编译不运行，快速暴露语法问题
  if ! "$HCC" -std=c++17 -O2 --offload-arch="$DCU_ARCH" -Isource/dcu \
        source/dcu/student_ops.cpp source/dcu/dcu_gmres.cpp source/dcu/test_student.cpp \
        -o build/student_test; then
    warn "后端自测程序编译失败，放弃策略 A"
    return 1
  fi
  say "后端自测程序编译通过：build/student_test"

  python3 source/tools/apply_dcu_patch.py || { warn "注入后端失败"; return 1; }

  local CC=gcc
  command -v gcc >/dev/null 2>&1 || CC="$HCC"   # 镜像无 gcc 时退回 hipcc

  if ! configure_xyce "$ROOT/build/xyce" "$CC" "$HCC" \
        "-DCMAKE_CXX_FLAGS=--offload-arch=$DCU_ARCH -O3" \
        "-DCMAKE_HIP_ARCHITECTURES=$DCU_ARCH"; then
    warn "策略 A 的 CMake 配置失败"
    return 1
  fi

  if ! cmake --build "$ROOT/build/xyce" -j "$JOBS"; then
    warn "策略 A 编译失败"
    return 1
  fi

  cmake --install "$ROOT/build/xyce" >/dev/null 2>&1
  [ -x "$ROOT/bin/Xyce" ]
}

# ---- 5. 策略 B：g++ / 纯 CPU 保底版 -------------------------------------
try_cpu_build() {
  say "策略 B：g++ 编译（纯 CPU 保底版）"
  python3 source/tools/apply_dcu_patch.py --revert >/dev/null 2>&1 || true

  local CC=gcc CXX=g++
  command -v gcc >/dev/null 2>&1 || CC=cc
  command -v g++ >/dev/null 2>&1 || CXX=c++

  if ! configure_xyce "$ROOT/build/xyce_cpu" "$CC" "$CXX"; then
    warn "策略 B 的 CMake 配置失败"
    return 1
  fi
  if ! cmake --build "$ROOT/build/xyce_cpu" -j "$JOBS"; then
    warn "策略 B 编译失败"
    return 1
  fi
  cmake --install "$ROOT/build/xyce_cpu" >/dev/null 2>&1
  [ -x "$ROOT/bin/Xyce" ]
}

# ---- 6. 执行 --------------------------------------------------------------
OK=0
if [ "$FORCE_CPU" = "1" ]; then
  warn "FORCE_CPU=1，跳过策略 A"
else
  if try_dcu_build; then OK=1; say "策略 A 成功（DCU 加速版）"; fi
fi
if [ "$OK" != "1" ]; then
  if try_cpu_build; then OK=1; say "策略 B 成功（纯 CPU 版）"; fi
fi

if [ "$OK" != "1" ]; then
  die "两种构建策略都失败，详见上方 cmake/编译器输出"
fi

# ---- 7. 校验 -------------------------------------------------------------
if [ ! -x "$ROOT/bin/Xyce" ]; then die "安装完成但 bin/Xyce 不可执行"; fi
say "产物：$ROOT/bin/Xyce"
"$ROOT/bin/Xyce" -v 2>&1 | head -3 || true
