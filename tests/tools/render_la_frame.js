/*
 * render_la_frame.js —— 把抓包解码出的 43×48 字节**反渲染成 128×64 图**（看清屏上实际被喂了什么）
 *
 * 反映射（与测试里 buildAddressMask 互逆）：
 *   扫描 s ↔ 时序 t = 42 - s；列 x = 3t + dx (dx=0..2)；off = (t 奇) ? 5-2dx : 2dx
 *   链位 k = 48*(y>>3) + 6*(y&7) + off  ⇒  byte = k>>3, bit = k&7
 *
 * 用法：node tests/tools/render_la_frame.js <csv> [第几个完整帧，默认最后一个] [latch=1|0]
 *   latch 默认 1 = 按真实面板建模（扫描 s 显示上一扫描锁存到的块，2026-10-08 LAT 极性更正后）
 */
'use strict';
const fs = require('fs');
const readline = require('readline');

const csv = process.argv[2];
const which = process.argv[3] ? parseInt(process.argv[3], 10) : -1;

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
    if (!frames.length) { console.log('没找到完整帧'); return; }
    const f0 = frames[(which + frames.length) % frames.length];
    console.log(`渲染帧起点扫描 ${f0}  t=${(scans[f0].t0 * 1e3).toFixed(1)} ms`);

    /* 解码该帧 43×48 字节 */
    const bytes = new Uint8Array(43 * 48);
    for (let s = 0; s < 43; ++s) {
        const sc = scans[f0 + s];
        if (sc.clocks.length !== 384) continue;
        for (let b = 0; b < 48; ++b) {
            let v = 0;
            for (let k = 0; k < 8; ++k) v |= (sc.clocks[b * 8 + k].sia & 1) << k;
            bytes[s * 48 + b] = v;
        }
    }
    /* ---- 面板锁存模型（2026-10-08 LAT 极性更正后才有意义）----
     * 列驱动的锁存发生在扫描边界的 LAT 有效电平期间，而该边界在**本扫描 48 字节移位之前**
     * ⇒ 扫描 s 期间屏幕上显示的是**上一扫描**移入的那一块。
     *   latch=1（默认）：按真实面板建模 ⇒ 用扫描 s−1 的字节；
     *   latch=0          ：旧（极性反相、LAT 长期空闲高=透明）的等效行为 ⇒ 用扫描 s 的字节。
     * 配合现版固件（scanPhase = −1，即"提前一个扫描发"），latch=1 才会渲染出正确画面。 */
    const latch = (process.argv[4] === undefined) ? 1 : (parseInt(process.argv[4], 10) ? 1 : 0);
    console.log(`面板锁存模型: latch=${latch}${latch ? '（扫描 s 显示扫描 s−1 锁存到的块，真实面板）' : '（透明/直通，旧极性下的等效行为）'}`);

    /* 反映射成 128×64 */
    const img = Array.from({ length: 64 }, () => new Array(128).fill(0));
    for (let s = 0; s < 43; ++s) {
        const t = 42 - s;
        const src = latch ? ((s - 1 + 43) % 43) : s;
        for (let y = 0; y < 64; ++y) {
            for (let dx = 0; dx < 3; ++dx) {
                const x = 3 * t + dx;
                if (x >= 128) continue;
                /* 槽位映射：与 vfd_scanpack.cpp 的**默认 mode 0**（2026-10-08 起）一致：
                 * t 偶 -> off = 5,3,1 ；t 奇 -> off = 0,2,4（原驱动取值的镜像） */
                const off = (t & 1) ? (2 * dx) : (5 - 2 * dx);
                const k = 48 * (y >> 3) + 6 * (y & 7) + off;
                img[y][x] = (bytes[src * 48 + (k >> 3)] >> (k & 7)) & 1;
            }
        }
    }
    let lit = 0;
    for (const row of img) for (const v of row) lit += v;
    console.log(`点亮像素 ${lit} / 8192`);
    for (const row of img) console.log(row.map((v) => (v ? '#' : '.')).join(''));
})();
