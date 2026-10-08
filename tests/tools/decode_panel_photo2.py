# -*- coding: utf-8 -*-
"""
decode_panel_photo2.py —— 把 VFD 面板照片解码成 128x64 点阵并与期望画面比对（v2）

相对 v1 的改进：
  · 面板区域用命令行传入（不同照片机位不同）
  · 亮度用 Otsu 自适应阈值（而不是固定倍数），抗相机曝光/眩光
  · 角点改为"在四个象限内找最远亮点"，并用四点最小二乘仿射拟合
  · 输出：点阵 ASCII、解码 PNG、与期望的叠加图（红=期望有/照片无，蓝=照片有/期望无，绿=一致）

用法：
  python decode_panel_photo2.py <照片> <期望.pgm> <输出前缀> <x0> <y0> <x1> <y1>   # 坐标为 1708 宽预览坐标
"""
import sys
import math
from PIL import Image

PREVIEW_W = 1708.0
W, H = 128, 64


def load_green(path, box_preview):
    im = Image.open(path).convert('RGB')
    s = im.size[0] / PREVIEW_W
    box = tuple(int(v * s) for v in box_preview)
    crop = im.crop(box)
    px = crop.load()
    w, h = crop.size
    g = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, gg, b = px[x, y]
            v = gg - (r + b) // 2
            g[y][x] = v if v > 0 else 0
    return crop, g, w, h


def otsu(vals):
    lo, hi = min(vals), max(vals)
    if hi - lo < 1:
        return lo + 1
    hist = [0] * 256
    for v in vals:
        hist[min(255, max(0, int((v - lo) * 255.0 / (hi - lo))))] += 1
    total = len(vals)
    sum_all = sum(i * hist[i] for i in range(256))
    sumB = 0.0
    wB = 0
    best, thr = -1.0, lo
    for i in range(256):
        wB += hist[i]
        if wB == 0:
            continue
        wF = total - wB
        if wF == 0:
            break
        sumB += i * hist[i]
        mB = sumB / wB
        mF = (sum_all - sumB) / wF
        between = wB * wF * (mB - mF) ** 2
        if between > best:
            best, thr = between, lo + (i + 1) * (hi - lo) / 255.0
    return thr


def corners(mask, w, h):
    pts = [(x, y) for y in range(h) for x in range(w) if mask[y][x]]
    if len(pts) < 200:
        return None
    q = lambda f: (lambda p: f(p))
    tl = min((p for p in pts if p[0] < w * .5 and p[1] < h * .5), key=lambda p: p[0] + p[1], default=None)
    tr = max((p for p in pts if p[0] > w * .5 and p[1] < h * .5), key=lambda p: p[0] - p[1], default=None)
    br = max((p for p in pts if p[0] > w * .5 and p[1] > h * .5), key=lambda p: p[0] + p[1], default=None)
    bl = min((p for p in pts if p[0] < w * .5 and p[1] > h * .5), key=lambda p: p[0] - p[1], default=None)
    if None in (tl, tr, br, bl):
        return None
    return [tl, tr, br, bl]


def solve3(A, b):
    def det(M):
        return (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
                - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]))
    D = det(A)
    if abs(D) < 1e-12:
        return None
    out = []
    for i in range(3):
        M = [r[:] for r in A]
        for r in range(3):
            M[r][i] = b[r]
        out.append(det(M) / D)
    return out


def affine(cp):
    ideal = [(0, 0), (W - 1, 0), (W - 1, H - 1), (0, H - 1)]
    res = []
    for comp in (0, 1):
        S = [[0.0] * 3 for _ in range(3)]
        t = [0.0] * 3
        for (ix, iy), p in zip(ideal, cp):
            row = [ix, iy, 1]
            for i in range(3):
                for j in range(3):
                    S[i][j] += row[i] * row[j]
                t[i] += row[i] * p[comp]
        res.append(solve3(S, t))
    return res


def decode(g, w, h, coef, thr_dot):
    ax, ay, ac = coef[0]
    bx, by, bc = coef[1]
    pitch = min(abs(ax), abs(by))
    rad = max(1, int(round(pitch * 0.28)))
    bm = [[0] * W for _ in range(H)]
    vals = [[0.0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            px, py = ax * x + ay * y + ac, bx * x + by * y + bc
            best = 0.0
            for dy in range(-rad, rad + 1):
                yy = int(round(py)) + dy
                if 0 <= yy < h:
                    row = g[yy]
                    for dx in range(-rad, rad + 1):
                        xx = int(round(px)) + dx
                        if 0 <= xx < w and row[xx] > best:
                            best = row[xx]
            vals[y][x] = best
    flat = sorted(v for row in vals for v in row)
    thr = otsu(flat)
    for y in range(H):
        for x in range(W):
            bm[y][x] = 1 if vals[y][x] >= thr else 0
    return bm, vals, thr, rad


def load_pgm(path):
    d = open(path, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = [int(v) for v in parts[1].split()]
    body = parts[3]
    return [[1 if body[y * w + x] > 127 else 0 for x in range(w)] for y in range(h)]


def art(bm):
    return '\n'.join(''.join('#' if v else '.' for v in r) for r in bm)


def save(bm, path, scale=7, color=(0, 255, 120)):
    im = Image.new('RGB', (W, H), (0, 0, 0))
    p = im.load()
    for y in range(H):
        for x in range(W):
            if bm[y][x]:
                p[x, y] = color
    im.resize((W * scale, H * scale), Image.NEAREST).save(path)


def overlay(exp, got, path, scale=7):
    im = Image.new('RGB', (W, H), (0, 0, 0))
    p = im.load()
    for y in range(H):
        for x in range(W):
            e, o = exp[y][x], got[y][x]
            p[x, y] = (70, 210, 100) if (e and o) else ((255, 70, 70) if e else ((70, 130, 255) if o else (0, 0, 0)))
    im.resize((W * scale, H * scale), Image.NEAREST).save(path)


def main():
    photo, exp_pgm, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    box = [float(v) for v in sys.argv[4:8]] if len(sys.argv) >= 8 else [175, 265, 1035, 700]
    crop, g, w, h = load_green(photo, box)
    crop.save(prefix + '_crop.png')
    vals_all = sorted(v for row in g for v in row)
    thr = otsu(vals_all)
    # 几何定位用"高分位"阈值，只保留明确点亮的点，避免面板外的绿色光晕把角点拉到裁剪边缘
    hi_thr = vals_all[int(len(vals_all) * 0.94)]
    mask = [[1 if g[y][x] >= hi_thr else 0 for x in range(w)] for y in range(h)]
    cp = corners(mask, w, h)
    if not cp:
        print('角点检测失败（高分位阈值 %.0f）' % hi_thr); return 1
    print('裁剪尺寸', (w, h), ' Otsu 阈值 %.0f  几何阈值 %.0f' % (thr, hi_thr))
    print('四角:', cp)
    coef = affine(cp)
    print('仿射: x=(%.4f,%.4f,%.1f) y=(%.4f,%.4f,%.1f)' % (coef[0][0], coef[0][1], coef[0][2], coef[1][0], coef[1][1], coef[1][2]))
    bm, v, dthr, rad = decode(g, w, h, coef, thr)
    print('点采样阈值 %.0f  采样半径 %d px  每点间距 ≈ %.2f px' % (dthr, rad, min(abs(coef[0][0]), abs(coef[1][1]))))
    open(prefix + '_decoded.txt', 'w', encoding='utf-8').write(art(bm))
    save(bm, prefix + '_decoded.png')
    exp = load_pgm(exp_pgm)
    open(prefix + '_expected.txt', 'w', encoding='utf-8').write(art(exp))
    overlay(exp, bm, prefix + '_overlay.png')
    lit_e = sum(map(sum, exp)); lit_g = sum(map(sum, bm))
    agree = sum(1 for y in range(H) for x in range(W) if exp[y][x] == bm[y][x])
    print('点亮像素 期望 %d / 解码 %d；逐点一致率 %.1f%%' % (lit_e, lit_g, 100.0 * agree / (W * H)))
    # 逐列/逐行统计（帮助定位是列方向还是行方向的问题）
    print('逐列差异 Top10（期望点亮数 vs 解码点亮数）:')
    diffs = []
    for x in range(W):
        e = sum(exp[y][x] for y in range(H)); o = sum(bm[y][x] for y in range(H))
        diffs.append((abs(e - o), x, e, o))
    diffs.sort(reverse=True)
    for d, x, e, o in diffs[:10]:
        print('   x=%3d  期望 %2d  解码 %2d' % (x, e, o))
    return 0


if __name__ == '__main__':
    sys.exit(main())
