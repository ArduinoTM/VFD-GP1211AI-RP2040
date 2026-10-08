/*
 * solve_bitorder.js —— 判定实际 SPI 数据流的位序/奇偶映射：4 种组合里哪种能还原出自检画面
 *
 * 组合：位序 LSB-first / MSB-first  ×  off 奇偶映射 {0,2,4|5,3,1} 或 {1,3,5|4,2,0}
 * 判据：与宿主机导出的期望画面（PGM）逐点一致率。
 *
 * 用法：node tests/tools/solve_bitorder.js <csv> <expected.pgm>
 */
'use strict';
const fs = require('fs');
const readline = require('readline');

const csv = process.argv[2];
const pgm = process.argv[3];

function loadPgm(path) {
    const d = fs.readFileSync(path);
    const parts = d.toString('latin1').split('\n');
    const [w, h] = parts[1].split(' ').map(Number);
    const body = d.slice(parts[0].length + parts[1].length + 3 + 2);
    const img = [];
    for (let y = 0; y < h; ++y) {
        const row = [];
        for (let x = 0; x < w; ++x) row.push(body[y * w + x] > 127 ? 1 : 0);
        img.push(row);
    }
    return img;
}

(async () => {
    const rl = readline.createInterface({ input: fs.createReadStream(csv), crlfDelay: Infinity });
    let hdr = null, prev = null;
    const clkRise = [], clkgRise = [], bkRise = [];
    for await (const line of rl) {
        if (!line) continue;
        const p = line.split(',');
        if (!hdr) { hdr = p.map((s) => s.trim()); continue; }
        const t = parseFloat(p[0]);
        const v = { CLKa: +p[1], SIa: +p[2], LAT: +p[3], CLKg: +p[4], SIg: +p[5], BK: +p[6] };
        if (prev) {
            if (prev.CLKa === 0 && v.CLKa === 1) clkRise.push({ t, sia: v.SIa });
            if (prev.CLKg === 0 && v.CLKg === 1) clkgRise.push({ t, sig: prev.SIg });
            if (prev.BK === 0 && v.BK === 1) bkRise.push({ t });
        }
        prev = v;
    }
    const scans = [];
    for (let i = 0; i < bkRise.length; ++i) {
        const t0 = bkRise[i].t, t1 = (i + 1 < bkRise.length) ? bkRise[i + 1].t : Infinity;
        scans.push({ t0, clocks: clkRise.filter((e) => e.t >= t0 && e.t < t1), pulses: clkgRise.filter((e) => e.t >= t0 && e.t < t1) });
    }
    const starts = [];
    scans.forEach((s, i) => { if (s.pulses.length === 6) starts.push(i); });
    const frames = [];
    for (let k = 1; k < starts.length; ++k) if (starts[k] - starts[k - 1] === 43) frames.push(starts[k - 1]);
    console.log(`完整帧 ${frames.length} 个`);
    const exp = loadPgm(pgm);

    const variants = [
        { name: 'LSB-first + off{0,2,4|5,3,1}', lsb: true, par: 0 },
        { name: 'MSB-first + off{0,2,4|5,3,1}', lsb: false, par: 0 },
        { name: 'LSB-first + off{1,3,5|4,2,0}', lsb: true, par: 1 },
        { name: 'MSB-first + off{1,3,5|4,2,0}', lsb: false, par: 1 },
    ];

    for (const v of variants) {
        let totalSame = 0, totalPix = 0, lit = 0;
        for (const f0 of frames.slice(-3)) {
            const bytes = new Uint8Array(43 * 48);
            for (let s = 0; s < 43; ++s) {
                const sc = scans[f0 + s];
                if (sc.clocks.length !== 384) continue;
                for (let b = 0; b < 48; ++b) {
                    let val = 0;
                    for (let k = 0; k < 8; ++k) {
                        const bit = sc.clocks[b * 8 + k].sia & 1;
                        if (v.lsb) val |= bit << k; else val |= bit << (7 - k);
                    }
                    bytes[s * 48 + b] = val;
                }
            }
            for (let s = 0; s < 43; ++s) {
                const t = 42 - s;
                for (let y = 0; y < 64; ++y) {
                    for (let dx = 0; dx < 3; ++dx) {
                        const x = 3 * t + dx;
                        if (x >= 128) continue;
                        const off = v.par === 0
                            ? ((t & 1) ? (5 - 2 * dx) : (2 * dx))
                            : ((t & 1) ? (4 - 2 * dx) : (1 + 2 * dx));
                        const k = 48 * (y >> 3) + 6 * (y & 7) + off;
                        const bitv = (bytes[s * 48 + (k >> 3)] >> (k & 7)) & 1;
                        totalPix++;
                        if (bitv === exp[y][x]) totalSame++;
                        lit += bitv;
                    }
                }
            }
        }
        console.log(`  ${v.name}: 一致率 ${(100 * totalSame / totalPix).toFixed(2)}%  平均点亮 ${(lit / (frames.slice(-3).length * 8192)).toFixed(4)}`);
        /* 对"点亮数正常"的变体再搜一次二维位移，看图像到底偏了几个点 */
        if (lit / (frames.slice(-3).length * 8192) > 0.01 && lit / (frames.slice(-3).length * 8192) < 0.30) {
            const f0 = frames[frames.length - 1];
            const bytes = new Uint8Array(43 * 48);
            for (let s = 0; s < 43; ++s) {
                const sc = scans[f0 + s];
                if (sc.clocks.length !== 384) continue;
                for (let b = 0; b < 48; ++b) {
                    let val = 0;
                    for (let k = 0; k < 8; ++k) {
                        const bit = sc.clocks[b * 8 + k].sia & 1;
                        if (v.lsb) val |= bit << k; else val |= bit << (7 - k);
                    }
                    bytes[s * 48 + b] = val;
                }
            }
            const img = Array.from({ length: 64 }, () => new Array(128).fill(0));
            for (let s = 0; s < 43; ++s) {
                const t = 42 - s;
                for (let y = 0; y < 64; ++y) for (let dx = 0; dx < 3; ++dx) {
                    const x = 3 * t + dx;
                    if (x >= 128) continue;
                    const off = v.par === 0 ? ((t & 1) ? (5 - 2 * dx) : (2 * dx)) : ((t & 1) ? (4 - 2 * dx) : (1 + 2 * dx));
                    const k = 48 * (y >> 3) + 6 * (y & 7) + off;
                    img[y][x] = (bytes[s * 48 + (k >> 3)] >> (k & 7)) & 1;
                }
            }
            let best = { r: -1, dx: 0, dy: 0 };
            for (let dy = -6; dy <= 6; ++dy) for (let dx = -9; dx <= 9; ++dx) {
                let same = 0, tot = 0;
                for (let y = 0; y < 64; ++y) {
                    const yy = y + dy; if (yy < 0 || yy >= 64) continue;
                    for (let x = 0; x < 128; ++x) {
                        const xx = x + dx; if (xx < 0 || xx >= 128) continue;
                        tot++; if (img[yy][xx] === exp[y][x]) same++;
                    }
                }
                const r = tot ? same / tot : 0;
                if (r > best.r) best = { r, dx, dy };
            }
            console.log(`      ↳ 该变体最佳位移 dx=${best.dx} dy=${best.dy} ⇒ 一致率 ${(best.r * 100).toFixed(2)}%`);
        }
    }
})();
