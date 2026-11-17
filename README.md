# wwhx_start — Xyce 到 DCU 的移植工程（第一版）

2026 先导杯校内赛：把开源电路仿真器 Xyce 移植到国产 DCU 单卡并做性能优化。
本目录严格对齐组委会要求的 Git 提交结构，可整目录直接修改、直接构建。

> 构建环境：`source/build.sh` 会自动探测 DTK（`/opt/dtk-26.04` 等）与 Trilinos
> （`/usr/local/trilinos-xyce` 等），可用 `DTK_ROOT` / `TRILINOS_DIR` 覆盖；
> 缺 Ninja 时自动回退 Unix Makefiles。评测镜像自带 Trilinos 15.1.1；
> 集群登录节点未装 Trilinos，完整构建只能在评测环境或 DCU 计算节点完成。

---

## 1. 目录结构

```
仓库根目录/
|-- build.sh                 评测入口：调 source/build.sh，校验 bin/Xyce
|-- run.sh                   本地编译与自测工具（不算评测入口）
|-- README.md                本说明
|-- bin/                     构建产物（Xyce）
|-- lib/                     随源码提交的额外依赖库
`-- source/
    |-- build.sh             实际编译脚本（可修改）
    |-- numerical_config.json 本地自测数值配置
    |-- dcu/                 DCU 后端与算子
    |   |-- student_ops.h        7 个算子接口声明
    |   |-- student_ops.cpp      HIP kernel / CPU 参考实现
    |   |-- dcu_gmres.h          求解器对外接口
    |   |-- dcu_gmres.cpp        右预条件 GMRES(m)
    |   |-- N_LAS_DCUSolver.h    Xyce 求解器适配层
    |   |-- N_LAS_DCUSolver.C    Xyce 求解器适配层实现
    |   `-- test_student.cpp     8 项数值自测
    |-- tools/
    |   `-- apply_dcu_patch.py   把后端注入 Xyce 源码树（幂等，可 --revert）
    `-- xyce/                Xyce 完整源码及 CMake 配置
```

---

## 2. 构建与运行

评测环境为 Ubuntu 22.04.5 + DTK 26.04（HIP dcc 25.10.0，DCU_ARCH=gfx936）
+ Trilinos 15.1.1（预装 CPU Serial 后端）。

```bash
bash build.sh            # 完整构建，产出 bin/Xyce
bash run.sh              # 只跑 8 项后端自测
bash run.sh grid.cir     # 用新求解器跑一个电路
```

可覆盖的环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `DTK_ROOT` | `/opt/dtk-26.04` | DTK 根目录，hipcc 在其 bin 下 |
| `TRILINOS_DIR` | `/usr/local/trilinos-xyce` | Trilinos 安装前缀 |
| `DCU_ARCH` | `gfx936` | 目标架构 |
| `JOBS` | `nproc` | 并行编译度 |

`build.sh` 的步骤：

1. 准备 `PATH` / `LD_LIBRARY_PATH` / `OMP_NUM_THREADS=8`；
2. 先用 `hipcc` 编一遍后端与自测，快速暴露语法与链接问题；
3. 执行 `source/tools/apply_dcu_patch.py`，把后端注入 Xyce 源码树；
4. `cmake` + `Ninja` 构建 Xyce，`install` 到仓库根，得到 `bin/Xyce`。

`LD_LIBRARY_PATH` 已按评测口径设为
`$DTK_ROOT/lib` + `$TRILINOS_DIR/lib` + `仓库根/lib`。

---

## 3. 移植是怎么接进去的

Xyce 的线性求解器是 `Xyce::Linear::Solver` 抽象类，由
`TranSolverFactory::create()` 按 `-linsolv <TYPE>` 分派。本工程新增
`DCUSolver : public Solver`，并注册类型名 `DCUGMRES`：

```bash
bin/Xyce -linsolv DCUGMRES grid.cir
```

`DCUSolver::doSolve()` 的数据流：

1. 逐行调用 `Matrix::getLocalRowView()`——它返回内部存储指针（零拷贝），
   直接按行拼成扁平 CSR；
2. 从 RHS 取右端项，以 0 为初值；
3. 调 `dcu_gmres_solve()` 在 DCU 上求解；
4. 解写回 LHS。

GMRES(m) 内层的向量运算全部落在 7 个算子接口上，便于统一替换与优化：

| GMRES 内动作 | 使用的接口 |
|---|---|
| `w = A z` | `student_spmv` |
| `h = w · V[i]` | `student_dot` |
| `V[i+1] = w / ‖w‖` | `student_norm2` |
| `w -= h V[i]`、`x += Z y` | `student_axpby` |
| Jacobi 预条件 `z = D⁻¹ v` | `student_mul` |

`student_gemv` 与 `student_spgemm` 是手册要求的接口，本版已实现并自测，
尚未进入 GMRES 主线。

---

## 4. 当前状态（第一版）

- 7 个算子：HIP kernel 已实现（`USE_HIP`），CPU 回退同语义，本地 8 项自测全过。
- `USE_HIP` 由 `__HIPCC__` 自动判定，用 `hipcc` 编译无需额外加宏。
- 求解器：右预条件 GMRES(m) + Jacobi，`restart=30`、`max_iter=500`、`tol=1e-10`。
- 校正口径：`bin/Xyce` 装到仓库根，符合手册"可执行文件在 bin/"的要求。
- **尚未验证**：本版尚未在评测镜像上真正跑过 `bash build.sh`，
  也还没有 `waveform.csv` 端到端对比与加速比数据。

### 已知的性能短板（下一步）

1. **每次 solve 都重建 CSR 并上传**：矩阵结构在 Newton 迭代间通常不变，
   应缓存 `dRow/dCol`，只更新 `dVal`；
2. **每个 `student_dot` 都同步一次**：GMRES 每步要做 j+1 次内积，
   应改为批量内积 / 融合归约，减少 kernel 启动与同步；
3. **进程启动与装载占比大**：参考量级为端到端约 0.37 s，其中求解仅约 0.06 s，
   约 0.3 s 花在进程启动与电路装载，优化要同时考虑这部分；
4. **稀疏模式固定时可用 SpMV 行分块 + 向量化**，或对多右端项做 batched；

---

## 5. 口径提醒

- 输出 `waveform.csv`，TIME 列 0–100 ns 共 101 点、严格递增，时间误差 1e-17 s；
- 归一化误差 `E = max|x−xCPU| / (A + R|xCPU|)`，电压 A=1e-10 V，电流 A=1e-12 A，R=1e-7，E≤1 判通过；
- 标准答案由原版 CPU Xyce 生成，不得硬编码、不得把计算挪到计时外；
- 计时为进程启动到输出写完的端到端 wall time，多次取中位数。
