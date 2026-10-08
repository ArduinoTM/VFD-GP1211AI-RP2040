# -*- coding: utf-8 -*-
"""
decode_panel_photo3.py —— VFD 面板照片 → 128x64 点阵（v3：投影定位 + 逐点局部对比）

相对 v2：
  · 面板边界用"行/列亮度投影峰值"定位（双线边框是最亮的长直线），不依赖阈值二值化
  · 逐点采样后，用"该点与同一行/列邻域中位亮度之差"做判定，抵消相机泛光与照度不均
  · 输出偏移诊断：每个"期望点亮"的像素，最近的实际亮点偏了多少（判断是整体移位、列交换还是行错位）

用法：python decode_panel_photo3.py <照片> <期望.pgm> <输出前缀> [x0 y0 x1 y1（1708 宽预览坐标）]
"""
import sys
from PIL import Image

PREVIEW_W = 1708.0
W, H = 128, 64


def load(path, box_preview):
    im = Image.open(path).convert('RGB')
    s = im.size[0] / PREVIEW_W
    box = tuple(int(v * s) for v in box_preview)
    crop = im.crop(box)
    px = crop.load()
    w, h = crop.size
    g = [[0.0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, gg, b = px[x, y]
            v = gg - (r + b) * 0.5
            g[y][x] = v if v > 0 else 0.0
    return crop, g, w, h


def profile_peak(vals, thresh_ratio=0.75):
    """在一维投影里找最强峰（返回索引与峰强）；用重心细化"""
    mx = max(vals)
    if mx <= 0:
        return 0, 0.0
    thr = mx * thresh_ratio
    idxs = [i for i, v in enumerate(vals) if v >= thr]
    if not idxs:
        i = vals.index(mx)
        return i, mx
    # 取包含最大值的连续段
    best = []
    cur = [idxs[0]]
    for i in idxs[1:]:
        if i == cur[-1] + 1:
            cur.append(i)
        else:
            if vals.index(mx) in cur:
                best = cur
            cur = [i]
    if not best and vals.index(mx) in cur:
        best = cur
    if not best:
        best = cur
    wsum = sum(vals[i] for i in best)
    c = sum(i * vals[i] for i in best) / wsum if wsum else best[0]
    return c, mx


def edges_from_projection(g, w, h):
    rows = [sum(g[y][x] for x in range(w)) for y in range(h)]
    cols = [sum(g[y][x] for y in range(h)) for x in range(w)]
    # 上半区找最上一根亮线（外框上边），下半区找最下一根（外框下边）
    half = h // 2
    top = max(range(half), key=lambda y: rows[y])
    bot = max(range(half, h), key=lambda y: rows[y])
    lefthalf = w // 2
    left = max(range(lefthalf), key=lambda x: cols[x])
    right = max(range(lefthalf, w), key=lambda x: cols[x])
    return (left, top, right, bot), rows, cols


def otsu(vals):
    lo, hi = min(vals), max(vals)
    if hi - lo < 1e-6:
        return lo + 1
    hist = [0] * 256
    for v in vals:
        hist[min(255, max(0, int((v - lo) * 255.0 / (hi - lo))))] += 1
    total = len(vals)
    sum_all = sum(i * hist[i] for i in range(256))
    sumB = 0.0; wB = 0; best = -1.0; thr = lo
    for i in range(256):
        wB += hist[i]
        if wB == 0: continue
        wF = total - wB
        if wF == 0: break
        sumB += i * hist[i]
        mB = sumB / wB
        mF = (sum_all - sumB) / wF
        b = wB * wF * (mB - mF) ** 2
        if b > best:
            best, thr = b, lo + (i + 1) * (hi - lo) / 255.0
    return thr


def art(bm):
    return '\n'.join(''.join('#' if v else '.' for v in r) for r in bm)


def save(bm, path, scale=7, color=(0, 255, 120)):
    im = Image.new('RGB', (W, H), (0, 0, 0)); p = im.load()
    for y in range(H):
        for x in range(W):
            if bm[y][x]: p[x, y] = color
    im.resize((W * scale, H * scale), Image.NEAREST).save(path)


def overlay(exp, got, path, scale=7):
    im = Image.new('RGB', (W, H), (0, 0, 0)); p = im.load()
    for y in range(H):
        for x in range(W):
            e, o = exp[y][x], got[y][x]
            p[x, y] = (70, 210, 100) if (e and o) else ((255, 70, 70) if e else ((70, 130, 255) if o else (0, 0, 0)))
    im.resize((W * scale, H * scale), Image.NEAREST).save(path)


def load_pgm(path):
    d = open(path, 'rb').read(); parts = d.split(b'\n', 3)
    w, h = [int(v) for v in parts[1].split()]; body = parts[3]
    return [[1 if body[y * w + x] > 127 else 0 for x in range(w)] for y in range(h)]


def panel_box(g, w, h, pct=0.90):
    """用"绿色增强后的高分位亮点"的稳健包围盒定位面板（双线边框即最外圈亮点）。
    取 0.5%/99.5% 分位并加一点内缩，避免个别反光点/噪点把框拉大。"""
    vals = sorted(v for row in g for v in row)
    thr = vals[int(len(vals) * pct)]
    xs, ys = [], []
    for y in range(h):
        row = g[y]
        for x in range(w):
            if row[x] >= thr:
                xs.append(x); ys.append(y)
    if len(xs) < 500:
        return None, thr
    xs.sort(); ys.sort()
    n = len(xs)
    lo = int(n * 0.002); hi = int(n * 0.998)
    return (xs[lo], ys[lo], xs[hi], ys[hi]), thr


def box_blur(g, w, h, r):
    """积分图实现的可分离盒式模糊（用于估计局部背景：相机泛光 + 照度不均）"""
    integ = [[0.0] * (w + 1) for _ in range(h + 1)]
    for y in range(h):
        row_sum = 0.0
        gi = integ[y + 1]
        gp = integ[y]
        grow = g[y]
        for x in range(w):
            row_sum += grow[x]
            gi[x + 1] = gp[x + 1] + row_sum
    out = [[0.0] * w for _ in range(h)]
    for y in range(h):
        y0 = max(0, y - r); y1 = min(h - 1, y + r)
        for x in range(w):
            x0 = max(0, x - r); x1 = min(w - 1, x + r)
            s = integ[y1 + 1][x1 + 1] - integ[y0][x1 + 1] - integ[y1 + 1][x0] + integ[y0][x0]
            out[y][x] = s / ((y1 - y0 + 1) * (x1 - x0 + 1))
    return out


def main():
    photo, exp_pgm, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    box = [float(v) for v in sys.argv[4:8]] if len(sys.argv) >= 8 else [130, 210, 1090, 745]
    crop, g, w, h = load(photo, box)
    crop.save(prefix + '_crop.png')
    pbox, pthr = panel_box(g, w, h)
    if not pbox:
        print('面板定位失败'); return 1
    x0, y0, x1, y1 = pbox
    print('面板包围盒 %s（裁剪 %dx%d，几何阈值 %.0f）' % (pbox, w, h, pthr))
    print('  边长 %d × %d px；点距 %.2f × %.2f px（应接近 1:1，否则说明包围盒没贴住边框）'
          % (x1 - x0, y1 - y0, (x1 - x0) / 127.0, (y1 - y0) / 63.0))
    # 局部背景（盒式模糊）扣除：让"点亮"与"泛光/背景"分离
    r = int(round(max((x1 - x0) / 127.0, (y1 - y0) / 63.0) * 4.0))
    bg = box_blur(g, w, h, r)
    d = [[max(0.0, g[y][x] - bg[y][x]) for x in range(w)] for y in range(h)]
    g = d
    print('  已扣除局部背景（模糊半径 %d px）' % r)
    pitch_x = (x1 - x0) / (W - 1.0)
    pitch_y = (y1 - y0) / (H - 1.0)
    print('  点距 ≈ %.2f × %.2f px' % (pitch_x, pitch_y))
    rx = max(1, int(round(pitch_x * 0.30)))
    ry = max(1, int(round(pitch_y * 0.30)))
    vals = [[0.0] * W for _ in range(H)]
    for y in range(H):
        cy = y0 + y * pitch_y
        for x in range(W):
            cx = x0 + x * pitch_x
            best = 0.0
            for dy in range(-ry, ry + 1):
                yy = int(round(cy)) + dy
                if 0 <= yy < h:
                    row = g[yy]
                    for dx in range(-rx, rx + 1):
                        xx = int(round(cx)) + dx
                        if 0 <= xx < w and row[xx] > best:
                            best = row[xx]
            vals[y][x] = best
    flat = sorted(v for r in vals for v in r)
    thr = otsu(flat)
    bm = [[1 if vals[y][x] >= thr else 0 for x in range(W)] for y in range(H)]
    print('  采样值 Otsu 阈值 %.1f；点亮 %d（期望 1500）' % (thr, sum(map(sum, bm))))
    open(prefix + '_decoded.txt', 'w', encoding='utf-8').write(art(bm))
    save(bm, prefix + '_decoded.png')
    if exp_pgm:
        exp = load_pgm(exp_pgm)
        open(prefix + '_expected.txt', 'w', encoding='utf-8').write(art(exp))
        overlay(exp, bm, prefix + '_overlay.png')
        # 最佳整数位移
        best = None
        for dy in range(-3, 4):
            for dx in range(-4, 5):
                a = t = 0
                for y in range(H):
                    yy = y + dy
                    if not (0 <= yy < H): continue
                    for x in range(W):
                        xx = x + dx
                        if not (0 <= xx < W): continue
                        t += 1
                        if exp[yy][xx] == bm[y][x]: a += 1
                if t and (best is None or a / t > best[0]): best = (a / t, dx, dy)
        print('  最佳位移 dx=%d dy=%d → 一致率 %.1f%%' % (best[1], best[2], best[0] * 100))
        # 偏移诊断：对每个"期望点亮"的点，找同行最近的实际亮点，记录其水平偏移
        hist = {}
        for y in range(H):
            xs = [x for x in range(W) if bm[y][x]]
            for x in range(W):
                if not exp[y][x]: continue
                if not xs:
                    hist['none'] = hist.get('none', 0) + 1
                    continue
                nearest = min(xs, key=lambda xo: abs(xo - x))
                d = nearest - x
                key = str(d) if abs(d) <= 4 else ('<%d' % d if d < 0 else '>%d' % d)
                hist[key] = hist.get(key, 0) + 1
        print('  期望亮点 → 同行最近实际亮点的水平偏移分布:',
              ', '.join('%s:%d' % (k, v) for k, v in sorted(hist.items(), key=lambda kv: -kv[1])[:10]))
        # 逐列点亮数对比（找出"多亮/少亮"的列带）
        diff = [(abs(sum(exp[y][x] for y in range(H)) - sum(bm[y][x] for y in range(H))), x,
                 sum(exp[y][x] for y in range(H)), sum(bm[y][x] for y in range(H))) for x in range(W)]
        diff.sort(reverse=True)
        print('  差异最大的列:', ', '.join('x=%d(期望%d/实际%d)' % (x, e, o) for _, x, e, o in diff[:8]))
        # 逐行
        diffr = [(abs(sum(exp[y]) - sum(bm[y])), y, sum(exp[y]), sum(bm[y])) for y in range(H)]
        diffr.sort(reverse=True)
        print('  差异最大的行:', ', '.join('y=%d(期望%d/实际%d)' % (y, e, o) for _, y, e, o in diffr[:8]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
