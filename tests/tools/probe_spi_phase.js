/*
 * probe_spi_phase.js —— 在抓包里找 CLKa 上升沿，打印其前后原始采样，确认 SIa 的采样相位
 * 用法：node tests/tools/probe_spi_phase.js <csv> <起始时间ms> [个数]
 */
'use strict';
const fs = require('fs');
const readline = require('readline');

const csv = process.argv[2];
const t0 = parseFloat(process.argv[3]) / 1000;
const n = parseInt(process.argv[4] || '6', 10);

(async () => {
    const rl = readline.createInterface({ input: fs.createReadStream(csv), crlfDelay: Infinity });
    let hdr = null, first = true, prev = null;
    const rows = [];
    let captured = 0, lastRise = -1;
    for await (const line of rl) {
        if (!line) continue;
        const p = line.split(',');
        if (first) { first = false; continue; }
        const t = parseFloat(p[0]);
        if (t < t0) { prev = { CLKa: +p[1], SIa: +p[2] }; continue; }
        if (captured >= n) break;
        if (prev && prev.CLKa === 0 && +p[1] === 1) {
            captured++;
            rows.push({ t, clka: +p[1], sia: +p[2], lat: +p[3], clkg: +p[4] });
        }
        prev = { CLKa: +p[1], SIa: +p[2] };
    }
    console.log(`从 ${(t0 * 1e3).toFixed(1)} ms 起的 ${rows.length} 个 CLKa 上升沿：`);
    for (const r of rows) {
        console.log(`  t=${(r.t * 1e3).toFixed(4)}ms  CLKa=1 SIa=${r.sia}  (同刻 CLKg=${r.clkg} LAT=${r.lat})`);
    }
    /* 再打印第一个上升沿附近 ±1.5 µs 的原始行，看 SIa 的跳变相对 CLKa 的位置 */
    if (rows.length) {
        const c = rows[0].t;
        console.log(`\n第一个上升沿附近 ±1.5 µs 的原始采样：`);
        const rl2 = readline.createInterface({ input: fs.createReadStream(csv), crlfDelay: Infinity });
        let f2 = true, cnt = 0;
        for await (const line of rl2) {
            if (!line) continue;
            const p = line.split(',');
            if (f2) { f2 = false; continue; }
            const t = parseFloat(p[0]);
            if (t < c - 1.5e-6) continue;
            if (t > c + 1.5e-6 || cnt > 40) break;
            cnt++;
            console.log(`  ${((t - c) * 1e9).toFixed(0).padStart(6)} ns  CLKa=${p[1]} SIa=${p[2]} LAT=${p[3]} CLKg=${p[4]} SIg=${p[5]} BK=${p[6]}`);
        }
    }
})();
