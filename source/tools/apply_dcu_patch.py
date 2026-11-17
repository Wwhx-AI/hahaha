#!/usr/bin/env python3
"""把 DCU 后端注入到 Xyce 源码树（幂等，可重复执行）。

做三件事：
  1. 把 source/dcu 下的后端文件复制到 Xyce 的 LinearAlgebraServicesPKG；
  2. 在 LinearAlgebraServicesPKG/CMakeLists.txt 的 target_sources 里加入新源文件；
  3. 在 N_LAS_TranSolverFactory.C 中注册 "-linsolv DCUGMRES" 分支。

用法：
  python3 source/tools/apply_dcu_patch.py            # 注入
  python3 source/tools/apply_dcu_patch.py --revert   # 回退（还原两个被改的文本文件）
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))            # 仓库根目录
DCU_DIR = os.path.join(ROOT, "source", "dcu")
XYCE_DIR = os.path.join(ROOT, "source", "xyce")
PKG_DIR = os.path.join(XYCE_DIR, "src", "LinearAlgebraServicesPKG")

BACKEND_SOURCES = [
    "N_LAS_DCUSolver.h",
    "N_LAS_DCUSolver.C",
    "student_ops.h",
    "student_ops.cpp",
    "dcu_gmres.h",
    "dcu_gmres.cpp",
]

CMAKE_FILE = os.path.join(PKG_DIR, "CMakeLists.txt")
FACTORY_FILE = os.path.join(PKG_DIR, "N_LAS_TranSolverFactory.C")

ANCHOR_SOURCES = "      ${KSPARSE_SRC}\n"
ANCHOR_INCLUDE = "#include <N_LAS_SimpleSolver.h>\n"
ANCHOR_DISPATCH = '  if( type == "AZTECOO" )\n'


def read(path):
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(text)


def copy_backend():
    if not os.path.isdir(DCU_DIR):
        sys.exit("找不到 %s" % DCU_DIR)
    for name in BACKEND_SOURCES:
        src = os.path.join(DCU_DIR, name)
        if not os.path.isfile(src):
            sys.exit("缺少后端文件：%s" % src)
        shutil.copy2(src, os.path.join(PKG_DIR, name))
    print("[patch] 已复制 %d 个后端文件到 %s" % (len(BACKEND_SOURCES), PKG_DIR))


def patch_cmake():
    text = read(CMAKE_FILE)
    if "N_LAS_DCUSolver.C" in text:
        print("[patch] CMakeLists.txt 已包含 DCU 源文件，跳过")
        return
    if ANCHOR_SOURCES not in text:
        sys.exit("CMakeLists.txt 里找不到锚点 ${KSPARSE_SRC}")
    insert = ("      N_LAS_DCUSolver.C\n"
              "      student_ops.cpp\n"
              "      dcu_gmres.cpp\n")
    text = text.replace(ANCHOR_SOURCES, insert + ANCHOR_SOURCES, 1)
    write(CMAKE_FILE, text)
    print("[patch] 已把 DCU 源文件加入 LinearAlgebraServicesPKG 的编译列表")


def patch_factory():
    text = read(FACTORY_FILE)
    changed = False

    if "#include <N_LAS_DCUSolver.h>" not in text:
        if ANCHOR_INCLUDE not in text:
            sys.exit("N_LAS_TranSolverFactory.C 里找不到 include 锚点")
        text = text.replace(
            ANCHOR_INCLUDE,
            "#include <N_LAS_DCUSolver.h>\n" + ANCHOR_INCLUDE, 1)
        changed = True

    if 'type == "DCUGMRES"' not in text:
        if ANCHOR_DISPATCH not in text:
            sys.exit("N_LAS_TranSolverFactory.C 里找不到分派锚点")
        text = text.replace(
            ANCHOR_DISPATCH,
            '  if( type == "DCUGMRES" )\n'
            "    return new DCUSolver( problem, options );\n"
            "  else if( type == \"AZTECOO\" )\n", 1)
        changed = True

    if changed:
        write(FACTORY_FILE, text)
        print('[patch] 已在 TranSolverFactory 注册 "-linsolv DCUGMRES"')
    else:
        print("[patch] TranSolverFactory 已注册 DCUGMRES，跳过")


def revert():
    text = read(CMAKE_FILE)
    if "N_LAS_DCUSolver.C" in text:
        write(CMAKE_FILE, text.replace(
            "      N_LAS_DCUSolver.C\n      student_ops.cpp\n      dcu_gmres.cpp\n",
            "", 1))
        print("[revert] CMakeLists.txt 已还原")

    text = read(FACTORY_FILE)
    if 'type == "DCUGMRES"' in text:
        text = text.replace(
            '  if( type == "DCUGMRES" )\n'
            "    return new DCUSolver( problem, options );\n"
            '  else if( type == "AZTECOO" )\n',
            ANCHOR_DISPATCH, 1)
        text = text.replace("#include <N_LAS_DCUSolver.h>\n", "", 1)
        write(FACTORY_FILE, text)
        print("[revert] N_LAS_TranSolverFactory.C 已还原")

    for name in BACKEND_SOURCES:
        p = os.path.join(PKG_DIR, name)
        if os.path.isfile(p):
            os.remove(p)
    print("[revert] 已删除注入的后端文件")


def main():
    if "--revert" in sys.argv:
        revert()
        return
    if not os.path.isdir(PKG_DIR):
        sys.exit("找不到 Xyce 源码：%s（请先把完整 Xyce 源码放到 source/xyce）" % PKG_DIR)
    copy_backend()
    patch_cmake()
    patch_factory()
    print("[patch] 完成")


if __name__ == "__main__":
    main()
