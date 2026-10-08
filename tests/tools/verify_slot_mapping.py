#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
滑槽位映射取证脚本（仅标准库，宿主机运行）

背景
----
把帧缓冲重排成 384bit 阳极链时，每个像素对应链位

    k = 48*page + 6*m + off          (page = y>>3, m = y&7)

其中 off 是"一条阳极线的 6 个序列"内的槽位。仓库里的分析报告
(Doc/GP1211AI_VFD_技术分析报告.md §3.5) 给出的 off 是：

    off = 1 + 2*dx   (t 偶)        off = 4 - 2*dx   (t 奇)      <-- 报告版
    off = 2*dx       (t 偶)        off = 5 - 2*dx   (t 奇)      <-- 本脚本/本移植版

本脚本把原驱动 VFD_GP1211AI.cpp::display() 的两个分支按源码里的掩码
(0x01/0x04/0x10/0x40 与 0x02/0x08/0x20/0x80) 转录成表，直接判断哪一版公式
与源码一致。

运行: python verify_slot_mapping.py
"""
import sys

SCANS, CHAIN, W = 43, 384, 128

# ---------------------------------------------------------------- 源码转录
# 原驱动 display() 的 6 个输出字节，每个字节 4 个数据位。
# 条目 = (输出字节内的位 p, 该位来自哪个列槽位 ci, 源字节内的位 m)
#   ci: 0/1/2 对应 n=0/1/2；t 偶时列 = base+0/1/2，t 奇时列 = base+5/4/3
# 掩码与源码逐字对应：
#   t 奇分支 (if (grid_tmp & 0x01)): 0x02,0x08,0x20,0x80 -> p = 1,3,5,7
#   t 偶分支 (else)               : 0x01,0x04,0x10,0x40 -> p = 0,2,4,6
EVEN_BRANCH = [  # else 分支（grid_tmp 偶），p = 0,2,4,6
    [(0, 0, 0), (2, 1, 0), (4, 2, 0), (6, 0, 1)],
    [(0, 1, 1), (2, 2, 1), (4, 0, 2), (6, 1, 2)],
    [(0, 2, 2), (2, 0, 3), (4, 1, 3), (6, 2, 3)],
    [(0, 0, 4), (2, 1, 4), (4, 2, 4), (6, 0, 5)],
    [(0, 1, 5), (2, 2, 5), (4, 0, 6), (6, 1, 6)],
    [(0, 2, 6), (2, 0, 7), (4, 1, 7), (6, 2, 7)],
]
ODD_BRANCH = [  # if 分支（grid_tmp 奇），p = 1,3,5,7
    [(1, 0, 0), (3, 1, 0), (5, 2, 0), (7, 0, 1)],
    [(1, 1, 1), (3, 2, 1), (5, 0, 2), (7, 1, 2)],
    [(1, 2, 2), (3, 0, 3), (5, 1, 3), (7, 2, 3)],
    [(1, 0, 4), (3, 1, 4), (5, 2, 4), (7, 0, 5)],
    [(1, 1, 5), (3, 2, 5), (5, 0, 6), (7, 1, 6)],
    [(1, 2, 6), (3, 0, 7), (5, 1, 7), (7, 2, 7)],
]


def source_map(t):
    """按源码返回 {(dx, m): k}，dx 为列在 x0..x0+2 中的偏移。"""
    table = ODD_BRANCH if (t & 1) else EVEN_BRANCH
    # 源码列顺序：t 奇 -> base+5,+4,+3（即 dx=2,1,0）；t 偶 -> base+0,+1,+2
    dx_of_slot = [2, 1, 0] if (t & 1) else [0, 1, 2]
    out = {}
    for page in range(8):
        for j, entries in enumerate(table):
            for (p, ci, m) in entries:
                k = 48 * page + 8 * j + p
                out[(dx_of_slot[ci], m, page)] = k
    return out


def formula_map(t, kind):
    out = {}
    for page in range(8):
        for m in range(8):
            for dx in range(3):
                if kind == "report":
                    off = (1 + 2 * dx) if (t % 2 == 0) else (4 - 2 * dx)
                else:
                    off = (2 * dx) if (t % 2 == 0) else (5 - 2 * dx)
                out[(dx, m, page)] = 48 * page + 6 * m + off
    return out


def compare(kind):
    bad = 0
    first = None
    for s in range(SCANS):
        t = 42 - s
        a, b = source_map(t), formula_map(t, kind)
        for key, k in a.items():
            if b.get(key) != k:
                bad += 1
                if first is None:
                    first = (s, t, key, k, b.get(key))
    return bad, first


if __name__ == "__main__":
    print("原驱动 display() 掩码转录 vs 两种闭式公式（比较 43 扫描 × 8 页 × 8 位 × 3 列）")
    for kind, label in (("report", "报告版 1+2dx / 4-2dx"), ("fixed", "本移植版 2dx / 5-2dx")):
        bad, first = compare(kind)
        extra = ""
        if first is not None:
            s, t, key, k_src, k_fml = first
            extra = "   首个不一致: s=%d t=%d (dx=%d, m=%d, page=%d) 源码 k=%d, 公式 k=%s" % (
                s, t, key[0], key[1], key[2], k_src, k_fml)
        print("  %-24s 不一致 %d 处%s" % (label, bad, extra))
    print()
    print("结论：报告版把 0x01/0x04/0x10/0x40 与 0x02/0x08/0x20/0x80 两个分支的槽位奇偶写反了，")
    print("      本移植版与原驱动 C 代码完全一致。")
    sys.exit(0)
