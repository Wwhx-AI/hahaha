#!/usr/bin/env bash
#-----------------------------------------------------------------------------
#  仓库根构建入口（评测器执行）
#
#  只做两件事：调用 source/build.sh 完成真正构建，然后校验产物 bin/Xyce。
#  评测口径要求 bin/Xyce 存在且可执行，否则整题计 0 分。
#-----------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "[build] 仓库根：$ROOT"
bash "$ROOT/source/build.sh" "$@"

if [ ! -f "$ROOT/bin/Xyce" ]; then
  echo "[build] 错误：未生成 bin/Xyce" >&2
  exit 1
fi
if [ ! -x "$ROOT/bin/Xyce" ]; then
  echo "[build] 错误：bin/Xyce 没有可执行权限" >&2
  exit 1
fi

echo "[build] 完成：$ROOT/bin/Xyce"
