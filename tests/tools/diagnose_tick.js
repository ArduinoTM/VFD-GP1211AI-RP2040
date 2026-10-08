/*
 * diagnose_tick.js —— 把两份抓包解码成图像，按行/按列找"tick 相对 pio 差在哪"
 * 用法：node tests/tools/diagnose_tick.js <pio.csv> <tick.csv>
 */
'use strict';
const fs = require('fs');
const readline = require('readline');
const SCANS = 43, BITS = 384, W = 128, H = 64;

async function frameOf(file) {
    const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
    let first = true, prev = null;
    let scanClocks = 0, scanPulses = 0, bitBuf = [], cur = [], lastFrame = null;
    for await (const line of rl) {
        if (!line) continue;
        const p = line.split(',');
        if (first) { first = false; continue; }
        const v = { CLKa: +p[1], SIa: +p[2], CLKg: +p[4], SIg: +p[5], BK: +p[6] };
        if (prev) {
            if (prev.CLKa === 0 && v.CLKa === 1) { scanClocks++; bitBuf.push(v.SIa & 1); }
            if (prev.CLKg === 0 && v.CLKg === 1) scanPulses++;
            if (prev.BK === 0 && v.BK === 1) {
                if (scanPulses === 6) cur = [];
                if (cur.length < SCANS) cur.push({ n: scanClocks, bits: bitBuf.slice() });
                if (cur.length === SCANS) lastFrame = cur;
                scanClocks = 0; scanPulses = 0; bitBuf = [];
            }
        }
        prev = v;
    }
    return lastFrame;
}

function toImage(frame) {
    const bits = new Uint8Array(SCANS * BITS);
    frame.forEach((s, i) => { for (let k = 0; k < BITS; ++k) bits[i * BITS + k] = s.bits[k] || 0; });
    const img = Array.from({ length: H }, () => new Array(W).fill(0));
    for (let s = 0; s < SCANS; ++s) {
        const t = 42 - s;
        for (let y = 0; y < H; ++y) for (let dx = 0; dx < 3; ++dx) {
            const x = 3 * t + dx;
            if (x >= W) continue;
            const off = (t & 1) ? (5 - 2 * dx) : (2 * dx);
            const k = 48 * (y >> 3) + 6 * (y & 7) + off;
            img[y][x] = bits[s * BITS + k];
        }
    }
    return img;
}

const litOf = (img) => img.reduce((a, r) => a + r.reduce((x, y) => x + y, 0), 0);

(async () => {
    const P = toImage(await frameOf(process.argv[2]));
    const T = toImage(await frameOf(process.argv[3]));
    console.log(`点亮像素：pio ${litOf(P)}   tick ${litOf(T)}   （自检画面期望 1533）`);

    const agree = (img1, img2, dy) => {
        let same = 0, tot = 0;
        for (let y = 0; y < H; ++y) {
            const yy = y + dy; if (yy < 0 || yy >= H) continue;
            for (let x = 0; x < W; ++x) { tot++; if (img1[yy][x] === img2[y][x]) same++; }
        }
        return tot ? same / tot : 0;
    };
    console.log(`\n整体一致率（tick vs pio）:`);
    let best = { dy: 0, dx: 0, r: -1 };
    for (let dy = -4; dy <= 4; ++dy) for (let dx = -6; dx <= 6; ++dx) {
        let same = 0, tot = 0;
        for (let y = 0; y < H; ++y) {
            const yy = y + dy; if (yy < 0 || yy >= H) continue;
            for (let x = 0; x < W; ++x) {
                const xx = x + dx; if (xx < 0 || xx >= W) continue;
                tot++; if (P[yy][xx] === T[y][x]) same++;
            }
        }
        const r = tot ? same / tot : 0;
        if (r > best.r) best = { dy, dx, r };
    }
    console.log(`  最佳位移 dy=${best.dy} dx=${best.dx} ⇒ ${(best.r * 100).toFixed(2)}%`);

    console.log(`\n逐行最佳水平位移（看是否存在"逐行漂移"= 剪切）:`);
    for (let y = 0; y < H; y += 4) {
        let bb = { dx: 0, r: -1 };
        for (let dx = -8; dx <= 8; ++dx) {
            let same = 0, tot = 0;
            for (let x = 0; x < W; ++x) {
                const xx = x + dx; if (xx < 0 || xx >= W) continue;
                tot++; if (P[y][xx] === T[y][x]) same++;
            }
            const r = tot ? same / tot : 0;
            if (r > bb.r) bb = { dx, r };
        }
        console.log(`  y=${String(y).padStart(2)}: 最佳 dx=${String(bb.dx).padStart(3)}  一致率 ${(bb.r * 100).toFixed(1)}%`);
    }

    console.log(`\n逐列最佳垂直位移:`);
    for (let x = 0; x < W; x += 16) {
        let bb = { dy: 0, r: -1 };
        for (let dy = -8; dy <= 8; ++dy) {
            let same = 0, tot = 0;
            for (let y = 0; y < H; ++y) {
                const yy = y + dy; if (yy < 0 || yy >= H) continue;
                tot++; if (P[yy][x] === T[y][x]) same++;
            }
            const r = tot ? same / tot : 0;
            if (r > bb.r) bb = { dy, r };
        }
        console.log(`  x=${String(x).padStart(3)}: 最佳 dy=${String(bb.dy).padStart(3)}  一致率 ${(bb.r * 100).toFixed(1)}%`);
    }
})();
