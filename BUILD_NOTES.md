# 构建与优化笔记（BUILD_NOTES）

记录实测结论、Xyce 官方构建依赖、以及后续 DCU 优化方向，供提交前自检用。

---

## 1. 实测：登录节点为什么构建不出来

`zz-login04`（Sugon OS，无 DCU）实测结果：

```
[build] DTK   : /public/software/compiler/dtk-26.04
[build] hipcc : /public/software/compiler/dtk-26.04/bin/hipcc   (dcc 25.10.0-0, clang 17)
[build] 错误：找不到 Trilinos 的 CMake 配置（lib/cmake/Trilinos）
```

单独跑 CMake 配置（不过 build.sh），失败点在 `cmake/tps.cmake:79`：

```
-- Could NOT find Xyce_Dependencies (missing: Xyce_Dependencies_DIR)
-- Looking for Trilinos
   Required packages: Amesos Epetra EpetraExt Ifpack NOX Teuchos Sacado
                      Triutils AztecOO Belos TrilinosCouplings
CMake Error at cmake/tps.cmake:79 (find_package):
  Could not find a package configuration file provided by "Trilinos"
```

结论：
- 登录节点全库扫描（`/opt`、`/public/software`、`VersionHub`、`mathlib`）都没有 Trilinos；
- 该账号 `sacctmgr` 的 Partition 为空，`sbatch -p hx1hnormal` 报 invalid partition，无法提交到 DCU 计算节点；
- 所以登录节点永远走不到 `bin/Xyce`。**这不是工程问题，是环境缺件。**

配置阶段其余部分正常：hipcc 被 CMake 当作 C/CXX 编译器接受，模型目录、`Xyce_Dependencies`（可选）都正常跳过。

---

## 2. Xyce 官方构建依赖（提交前必须核对）

Serial Xyce 要求 Trilinos **必须**包含：

- 包：`Amesos Epetra EpetraExt Ifpack NOX Teuchos Sacado Triutils AztecOO Belos TrilinosCouplings`
- 特性（缺一个 `cmake/tps.cmake:264` 直接 `FATAL_ERROR`）：
  - `NOX_ENABLE_LOCA`（目标 `NOX::loca`）
  - `Amesos_ENABLE_KLU`
  - `EpetraExt_BUILD_BTF`
  - `EpetraExt_BUILD_EXPERIMENTAL`
  - `EpetraExt_BUILD_GRAPH_REORDERINGS`
  - `Trilinos_ENABLE_COMPLEX_DOUBLE`
- 可选：`BLAS::all_libs`、`LAPACK::all_libs`（缺失只警告，不致命）
- OpenMP：只有 Trilinos 的 `Kokkos_DEVICES` 含 OPENMP 时才 `find_package(OpenMP REQUIRED)`，CPU Serial 后端通常会跳过

组委会镜像声明 Trilinos 15.1.1（Tpetra/Belos/Ifpack/Amesos，CPU Serial 后端）。若镜像里的 Trilinos 缺上述任一特性，配置会立刻 fatal，报错信息是
`Halting the Xyce configure due to missing features in Trilinos.`——遇到这句就是 Trilinos 编译选项不满足，不是我们的代码问题。

---

## 3. 提交前自检清单

1. Gitee 仓库公开、默认分支含 `build.sh`、`source/`（当前 = `084ae1b`，1458 文件、约 70 MB）；
2. 文件总数体积 < 100 MB（已删 `source/xyce/doc`、`utils/ADMS/examples` 大示例，仅留 `toys`）；
3. 评测环境的 DTK 前缀 `/opt/dtk-26.04`、Trilinos 前缀 `/usr/local/trilinos-xyce` 都被 `source/build.sh` 自动探测覆盖，无需手工传参；
4. 若平台日志出现 `missing features in Trilinos`，改传 `-DTrilinos_DIR=<镜像内实际 Trilinos 前缀>` 或换用满足条件的 Trilinos 版本。

---

## 4. 构建耗时风险

主树用 `hipcc` 作 C/CXX 编译器，会编译全部 Xyce 源文件（约 500+ TU）。8 核下预估 20–40 分钟，评测入口上限 2400 s，存在超时风险。可选降险方案（未验证，谨慎启用）：

- 主树改用系统 `g++`，仅把 `N_LAS_DCUSolver.C / student_ops.cpp / dcu_gmres.cpp` 标成 HIP 语言编译（需在 Xyce 顶层 `project()` 后 `enable_language(HIP)` 并给 3 个文件设 `LANGUAGE HIP`）；
- 或先用 `cmake --build -j8` 的增量缓存，多次提交不重建。

---

## 5. DCU 侧优化要点（来自《DCU 编程实战 2022》第 2 章）

- **wavefront = 64**（比 NVIDIA 的 32 宽），分支发散代价更高，kernel 内尽量无分支；
- **LDS 共享内存**每 CU 64 KB、32 个 bank、每 bank 4 Byte：SpMV/gemv 做 LDS 分块时要按 4 字节错开，避免 bank conflict；
- **寄存器**每线程 256 个向量寄存器，密集计算优先放寄存器，减少全局内存往返；
- **全局内存带宽 ~1 TB/s（HBM2）**，但延迟高，瓶颈通常在数据读取：优先做数据重用（按行分块、复用 x 向量）；
- **2 个拷贝引擎**可同时收发：把 `hipMemcpy` 拆到两个 stream，与 kernel 重叠；
- HIP 是 CUDA 的等价模型，`hipcc --offload-arch=gfx936` 单架构编译可显著减小体积与编译时间。

---

## 6. Xyce 加速的架构选择（搜到官方资料后）

- Xyce 现有线性代数层是 **Epetra**（我们注入的 `DCUSolver` 走的就是这条），兼容性最好；
- Trilinos 新一代栈是 **Tpetra + Kokkos**，Kokkos 已有 CUDA/HIP 后端，是"让 Xyce 真正跑在加速卡上"的正路，但需要改 Xyce 的求解器适配层，工作量大、风险高；
- Xyce 官方 FAQ 提到中等规模问题可用 `-linsolv klu`（直接解法）作对照基线，评估时可用它对比我们的 `DCUGMRES` 的收敛步数与耗时。

---

## 7. 下一步

- [ ] 在评测入口提交，抓 `build.sh` 完整日志，确认是否出现 `missing features in Trilinos`；
- [ ] 日志通过后，跑公开算例，记录 `DCUGMRES` 的迭代次数与墙上时间；
- [ ] 用 `hipprof` 找热点（实验三 02_gpu_hipprof），确认时间是在 SpMV、预条件还是数据传输；
- [ ] 按第 5 节做 SpMV 的 LDS 分块与向量化，目标是把 A 的一次乘加压到带宽上限附近。
