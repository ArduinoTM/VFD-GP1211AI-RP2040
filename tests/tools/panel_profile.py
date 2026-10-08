# -*- coding: utf-8 -*-
"""
panel_profile.py —— 在照片上取"垂直亮度剖面"，判断相邻行是"真点亮"还是"泛光/模糊"

对每个 x 打印 rows 0..12 的采样值（已扣局部背景）与期望位，用于分辨：
  · 期望亮 / 实际亮  → 正常
  · 期望暗 / 实际亮  → 相邻行被点亮（显示自身串行？还是相机泛光？看相对强度）
  · 期望亮 / 实际暗  → 漏点亮
"""
import os, sys
from PIL import Image

# 同目录导入（不写死本机路径）：用法 python panel_profile.py <照片> <expected.pgm> [box...] [xs]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode_panel_photo3 import load, panel_box, box_blur, load_pgm, W, H

photo = sys.argv[1]
exp_pgm = sys.argv[2]
box = [float(v) for v in sys.argv[3:7]] if len(sys.argv) >= 7 else [130, 210, 1090, 745]
xs = [int(v) for v in (sys.argv[7].split(',') if len(sys.argv) > 7 else ['30', '60', '100'])]

crop, g, w, h = load(photo, box)
(x0, y0, x1, y1) = panel_box(g, w, h)[0]
pitch_x = (x1 - x0) / (W - 1.0)
pitch_y = (y1 - y0) / (H - 1.0)
r = int(round(max(pitch_x, pitch_y) * 4.0))
bg = box_blur(g, w, h, r)
g = [[max(0.0, g[y][x] - bg[y][x]) for x in range(w)] for y in range(h)]
exp = load_pgm(exp_pgm)
rx = max(1, int(round(pitch_x * 0.30)))
ry = max(1, int(round(pitch_y * 0.30)))


def sample(x, y):
    cx, cy = x0 + x * pitch_x, y0 + y * pitch_y
    best = 0.0
    for dy in range(-ry, ry + 1):
        yy = int(round(cy)) + dy
        if 0 <= yy < h:
            for dx in range(-rx, rx + 1):
                xx = int(round(cx)) + dx
                if 0 <= xx < w and g[yy][xx] > best:
                    best = g[yy][xx]
    return best


for x in xs:
    print('\n=== x = %d 的垂直剖面（rows 0..14）===' % x)
    vals = [sample(x, y) for y in range(15)]
    mx = max(vals) or 1.0
    print('  row :  实际值(归一化)      期望')
    for y in range(15):
        bar = '#' * int(round(vals[y] / mx * 28))
        print('  %3d :  %7.1f  %5.0f%%  %s   %s' % (y, vals[y], 100 * vals[y] / mx, bar.ljust(28), '亮' if exp[y][x] else '·'))
