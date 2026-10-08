# -*- coding: utf-8 -*-
"""
decode_panel_photo.py —— 把 VFD 面板照片解码成 128x64 点阵，并与"应有画面"比对

思路：
  1) 裁剪面板区域（含双线边框）→ 阈值提取亮点
  2) 用四个角点（双线边框的外框）做仿射拟合：理想坐标 (0,0)..(127,63) → 照片坐标
  3) 在映射后的每个格点邻域取最大亮度 → 亮/灭 → 128x64 点阵
  4) 与宿主机导出的期望点阵做逐点比对（并搜索最佳水平/垂直位移），输出 ASCII 与 PNG
"""
import os, sys, math
from PIL import Image

# 用法：python decode_panel_photo.py <面板照片> [expected.pgm] [输出前缀]
# 三个路径都可用参数给；不给时退回"本脚本目录下的同名文件"，避免写死任何本机绝对路径。
_here = os.path.dirname(os.path.abspath(__file__))
PHOTO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_here, 'panel_photo.jpg')
EXPECT_PGM = sys.argv[2] if len(sys.argv) > 2 else os.path.join(_here, 'expected.pgm')
OUT_PREFIX = sys.argv[3] if len(sys.argv) > 3 else os.path.join(_here, 'photo')
# 预览坐标(1708x961)下的面板区域，按原图比例放大；可按需微调
BOX_PREVIEW = (372, 392, 1158, 818)
PREVIEW_W = 1708.0

W, H = 128, 64

def load_gray_green(path):
    im = Image.open(path).convert('RGB')
    s = im.size[0] / PREVIEW_W
    box = tuple(int(v * s) for v in BOX_PREVIEW)
    crop = im.crop(box)
    px = crop.load()
    w, h = crop.size
    g = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, gg, b = px[x, y]
            # 绿屏：用"绿色分量减去其它分量"增强，抑制蓝色灯条与白色手部
            v = gg - (r + b) // 2
            g[y][x] = v if v > 0 else 0
    return crop, g, w, h

def threshold(g, w, h, k=2.2):
    vals = sorted(v for row in g for v in row if v > 0)
    if not vals:
        return [[0] * w for _ in range(h)], 0
    med = vals[len(vals) // 2]
    thr = max(40, med * k)
    m = [[1 if g[y][x] >= thr else 0 for x in range(w)] for y in range(h)]
    return m, thr

def corners(m, w, h):
    """在四个象限内各取最极端亮点作为边框外角点"""
    pts = [(x, y) for y in range(h) for x in range(w) if m[y][x]]
    if len(pts) < 100:
        return None
    c = []
    # 左上：min(x+y) 但限制在左侧 / 上侧区域，避免噪声
    c.append(min((p for p in pts if p[0] < w * 0.5 and p[1] < h * 0.5), key=lambda p: p[0] + p[1]))
    c.append(max((p for p in pts if p[0] > w * 0.5 and p[1] < h * 0.5), key=lambda p: p[0] - p[1]))
    c.append(max((p for p in pts if p[0] > w * 0.5 and p[1] > h * 0.5), key=lambda p: p[0] + p[1]))
    c.append(min((p for p in pts if p[0] < w * 0.5 and p[1] > h * 0.5), key=lambda p: p[0] - p[1]))
    return c

def solve3(A, b):
    """3x3 线性方程（Cramer）"""
    def det(M):
        return (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
                - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]))
    D = det(A)
    if abs(D) < 1e-9:
        return None
    out = []
    for i in range(3):
        M = [row[:] for row in A]
        for r in range(3):
            M[r][i] = b[r]
        out.append(det(M) / D)
    return out

def affine_from(corner_pts):
    """理想角 (0,0),(127,0),(127,63),(0,63) -> 照片坐标；最小二乘仿射"""
    ideal = [(0, 0), (W - 1, 0), (W - 1, H - 1), (0, H - 1)]
    # 每个输出分量：a*x + b*y + c
    coef = []
    for comp in (0, 1):
        S = [[0.0] * 3 for _ in range(3)]
        t = [0.0] * 3
        for (ix, iy), p in zip(ideal, corner_pts):
            row = [ix, iy, 1]
            for i in range(3):
                for j in range(3):
                    S[i][j] += row[i] * row[j]
                t[i] += row[i] * p[comp]
        coef.append(solve3(S, t))
    return coef  # [[ax,ay,ac],[bx,by,bc]]

def sample(g, w, h, coef, rad):
    ax, ay, ac = coef[0]
    bx, by, bc = coef[1]
    out = [[0] * W for _ in range(H)]
    vals = [[0.0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            px = ax * x + ay * y + ac
            py = bx * x + by * y + bc
            best = 0.0
            for dy in range(-rad, rad + 1):
                yy = int(round(py)) + dy
                if yy < 0 or yy >= h:
                    continue
                row = g[yy]
                for dx in range(-rad, rad + 1):
                    xx = int(round(px)) + dx
                    if 0 <= xx < w and row[xx] > best:
                        best = row[xx]
            vals[y][x] = best
    flat = sorted(v for row in vals for v in row)
    thr = flat[int(len(flat) * 0.55)]
    for y in range(H):
        for x in range(W):
            out[y][x] = 1 if vals[y][x] >= thr else 0
    return out, vals, thr

def load_pgm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # P5\n<w> <h>\n255\n
    parts = data.split(b'\n', 3)
    w, h = [int(v) for v in parts[1].split()]
    body = parts[3]
    return [[1 if body[y * w + x] > 127 else 0 for x in range(w)] for y in range(h)], w, h

def ascii_art(bm):
    return '\n'.join(''.join('#' if v else '.' for v in row) for row in bm)

def best_shift(exp, got):
    best = None
    for dy in range(-4, 5):
        for dx in range(-6, 7):
            agree = tot = 0
            for y in range(H):
                yy = y + dy
                if yy < 0 or yy >= H:
                    continue
                for x in range(W):
                    xx = x + dx
                    if xx < 0 or xx >= W:
                        continue
                    tot += 1
                    if exp[yy][xx] == got[y][x]:
                        agree += 1
            if tot and (best is None or agree / tot > best[0]):
                best = (agree / tot, dx, dy, agree, tot)
    return best

def save_png(bm, path, scale=6):
    im = Image.new('RGB', (W, H), (0, 0, 0))
    p = im.load()
    for y in range(H):
        for x in range(W):
            if bm[y][x]:
                p[x, y] = (0, 255, 120)
    im.resize((W * scale, H * scale), Image.NEAREST).save(path)

def save_overlay(exp, got, path, scale=6):
    im = Image.new('RGB', (W, H), (0, 0, 0))
    p = im.load()
    for y in range(H):
        for x in range(W):
            e, g_ = exp[y][x], got[y][x]
            if e and g_:
                p[x, y] = (60, 200, 90)     # 两者都亮
            elif e:
                p[x, y] = (255, 60, 60)     # 只应有亮（缺）
            elif g_:
                p[x, y] = (60, 120, 255)    # 只照片亮（多）
    im.resize((W * scale, H * scale), Image.NEAREST).save(path)

def main():
    crop, g, w, h = load_gray_green(PHOTO)
    crop.save(OUT_PREFIX + '_crop.png')
    m, thr = threshold(g, w, h)
    cp = corners(m, w, h)
    if not cp:
        print('未找到边框角点 ✗')
        return 1
    print('边框外角点（照片裁剪坐标）:', cp, ' 裁剪尺寸', (w, h), ' 阈值', thr)
    coef = affine_from(cp)
    if not coef:
        print('仿射拟合失败 ✗')
        return 1
    pitch = abs(coef[0][0])
    print('每点横向间距 ≈ %.2f px，纵向 ≈ %.2f px' % (abs(coef[0][0]), abs(coef[1][1])))
    rad = max(1, int(round(min(abs(coef[0][0]), abs(coef[1][1])) * 0.30)))
    got, vals, vthr = sample(g, w, h, coef, rad)
    save_png(got, OUT_PREFIX + '_decoded.png')
    open(OUT_PREFIX + '_decoded.txt', 'w', encoding='utf-8').write(ascii_art(got))
    if EXPECT_PGM:
        exp, ew, eh = load_pgm(EXPECT_PGM)
        open(OUT_PREFIX + '_expected.txt', 'w', encoding='utf-8').write(ascii_art(exp))
        save_overlay(exp, got, OUT_PREFIX + '_overlay.png')
        bs = best_shift(exp, got)
        full = sum(1 for y in range(H) for x in range(W) if exp[y][x] == got[y][x]) / (W * H)
        print('逐点一致率（原样）: %.1f%%' % (full * 100))
        print('最佳位移 dx=%d dy=%d → 一致率 %.1f%% (%d/%d)' % (bs[1], bs[2], bs[0] * 100, bs[3], bs[4]))
        lit_exp = sum(sum(r) for r in exp)
        lit_got = sum(sum(r) for r in got)
        print('点亮像素：期望 %d，照片解码 %d' % (lit_exp, lit_got))
    print('窗口内亮度阈值(自适应) = %.0f' % vthr)
    return 0

if __name__ == '__main__':
    sys.exit(main())
