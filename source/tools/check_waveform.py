#!/usr/bin/env python3
"""校验 waveform.csv 是否符合评测口径，可选与参考结果比对误差。

用法：
  python3 source/tools/check_waveform.py waveform.csv
  python3 source/tools/check_waveform.py waveform.csv --reference ref.csv

校验项（对应手册第 3 节）：
  * 表头唯一、列数一致、TIME 列存在
  * TIME 非负严格递增，采样点数与名义间隔
  * 全部数值有限（无 NaN / Inf / 缺失）
  * 可选：归一化误差 E = max|x-xref| / (A + R*|xref|)，E<=1 判通过
"""
import argparse
import csv
import math
import sys

RELATIVE_R = 1e-7
ABS_VOLTAGE = 1e-10
ABS_CURRENT = 1e-12


def read_table(path):
    with open(path, "r", newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    if not rows:
        raise SystemExit("空文件：%s" % path)

    header = [h.strip() for h in rows[0]]
    if len(header) != len(set(h.upper() for h in header)):
        raise SystemExit("表头存在重复列名")

    data = []
    for r in rows[1:]:
        if not r or r[0].startswith("End"):
            continue
        if len(r) != len(header):
            raise SystemExit("行列数不一致：%r" % (r,))
        data.append([float(v) for v in r])
    return header, data


def check_basic(path, expect_points=None, expect_dt=None):
    header, data = read_table(path)
    upper = [h.upper() for h in header]

    if "TIME" not in upper:
        raise SystemExit("缺少 TIME 列")
    tcol = upper.index("TIME")

    if not data:
        raise SystemExit("没有数据行")

    for row in data:
        for v in row:
            if not math.isfinite(v):
                raise SystemExit("存在非有限值：%r" % (row,))

    times = [row[tcol] for row in data]
    if times[0] < 0:
        raise SystemExit("TIME 存在负值")
    for a, b in zip(times, times[1:]):
        if not b > a:
            raise SystemExit("TIME 未严格递增：%r -> %r" % (a, b))

    print("[ok] 采样点数 %d，TIME %.6g -> %.6g" % (len(times), times[0], times[-1]))
    if expect_points is not None and len(times) != expect_points:
        raise SystemExit("采样点数应为 %d，实际 %d" % (expect_points, len(times)))
    if expect_dt is not None and len(times) > 1:
        dts = [b - a for a, b in zip(times, times[1:])]
        drift = max(abs(d - expect_dt) for d in dts)
        print("[ok] 名义间隔 %.6g，最大偏离 %.3g" % (expect_dt, drift))
    return header, data


def compare(header, data, ref_path):
    ref_header, ref_data = read_table(ref_path)
    if len(ref_data) != len(data):
        raise SystemExit("与参考的采样点数不一致")

    ref_upper = [h.upper() for h in ref_header]

    worst = 0.0
    worst_col = ""
    for name in ref_upper:
        if name in ("TIME", "INDEX"):
            continue
        if name not in [h.upper() for h in header]:
            raise SystemExit("缺少物理量列：%s" % name)
        i = [h.upper() for h in header].index(name)
        j = ref_upper.index(name)
        atol = ABS_CURRENT if name.startswith("I(") else ABS_VOLTAGE
        for row_cur, row_ref in zip(data, ref_data):
            expected = row_ref[j]
            scaled = abs(row_cur[i] - expected) / (atol + RELATIVE_R * abs(expected))
            if scaled > worst:
                worst = scaled
                worst_col = name

    verdict = "PASS" if worst <= 1.0 else "FAIL"
    print("[%s] 最大归一化误差 E=%.3e（列 %s），阈值 1.0" % (verdict, worst, worst_col))
    return worst <= 1.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("waveform")
    ap.add_argument("--reference", default=None)
    ap.add_argument("--points", type=int, default=None)
    ap.add_argument("--dt", type=float, default=None)
    args = ap.parse_args()

    header, data = check_basic(args.waveform, args.points, args.dt)
    if args.reference:
        ok = compare(header, data, args.reference)
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
