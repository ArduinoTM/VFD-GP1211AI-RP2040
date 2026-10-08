/*
 * tick_pulse_widths.js —— 量 tick 版抓包里 CLKg 每个脉冲的低/高电平宽度
 *
 * 动机：tick 引擎的 CLKg 是"软件 strobe"（拉低 → 等 1 µs → 拉高）。
 *       相邻两次 strobe 之间若没有额外延时，那么**高电平只有两次调用之间的几条指令时间**，
 *       远小于手册对时钟/锁存脉冲宽度的要求；这会让面板的移位寄存器数不清时钟。
 *
 * 用法：node tests/tools/tick_pulse_widths.js <csv>
 */
'use strict';
const fs = require('fs');
const readline = require('readline');

const file = process.argv[2];

function bucket(list) {
    const m = new Map();
    for (const x of list) {
        const k = Math.round(x / 100) * 100;
        m.set(k, (m.get(k) || 0) + 1);
    }
    return [...m.entries()].sort((a, b) => a[0] - b[0]).map(([k, n]) => `${k}ns×${n}`).join(', ');
}

(async () => {
    const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
    let first = true, prev = null, lastClkFall = null, lastClkRise = null;
    const pulses = [];   // { rise, lowNs, highNs, sig, lat, bk }
    const lastSigEdge = { t: null, to: null };
    const latEdges = [];

    for await (const line of rl) {
        if (!line) continue;
        const p = line.split(',');
        if (first) { first = false; continue; }
        const t = parseFloat(p[0]);
        const v = { CLKa: +p[1], SIa: +p[2], LAT: +p[3], CLKg: +p[4], SIg: +p[5], BK: +p[6] };
        if (prev) {
            if (v.SIg !== prev.SIg) { lastSigEdge.t = t; lastSigEdge.to = v.SIg; }
            if (prev.CLKg === 1 && v.CLKg === 0) lastClkFall = t;          /* 进入低电平 */
            if (prev.CLKg === 0 && v.CLKg === 1 && lastClkFall !== null) { /* 上升沿：一个脉冲结束 */
                const sigBefore = lastSigEdge.t !== null && lastSigEdge.t <= lastClkFall ? lastSigEdge.to : prev.SIg;
                pulses.push({ rise: t, lowNs: (t - lastClkFall) * 1e9, sig: sigBefore });
                lastClkFall = null;
            }
            if (prev.LAT === 1 && v.LAT === 0) latEdges.push({ t, clkg: v.CLKg });
        }
        prev = v;
    }

    /* 高电平宽度：相邻两个上升沿之间减去后一个的低电平宽度 */
    for (let i = 1; i < pulses.length; ++i) {
        const gapNs = (pulses[i].rise - pulses[i - 1].rise) * 1e9;
        pulses[i - 1].highNs = gapNs - pulses[i].lowNs;
        pulses[i - 1].riseGapNs = gapNs;
    }

    const seedLike = pulses.filter((p) => p.highNs !== undefined && p.highNs < 150);   /* 高电平极短 → 疑似播种脉冲 */
    const normal = pulses.filter((p) => p.highNs !== undefined && p.highNs >= 150);
    console.log(`文件: ${file}`);
    console.log(`CLKg 脉冲总数 ${pulses.length}（其中能算出高电平的 ${pulses.length - 1}）`);
    console.log(`  低电平宽度分布: ${bucket(pulses.map((p) => p.lowNs))}`);
    console.log(`  高电平宽度分布: ${bucket(pulses.filter((p) => p.highNs !== undefined).map((p) => p.highNs))}`);
    console.log(`  上升沿间隔分布: ${bucket(pulses.filter((p) => p.riseGapNs !== undefined).map((p) => p.riseGapNs))}`);
    console.log(`  高电平 <150 ns 的"窄脉冲" ${seedLike.length} 个；>=150 ns 的 ${normal.length} 个`);
    if (seedLike.length) {
        const ns = seedLike.map((p) => p.highNs).sort((a, b) => a - b);
        console.log(`  窄脉冲高电平: min ${ns[0].toFixed(0)} ns  中位 ${ns[(ns.length / 2) | 0].toFixed(0)} ns  max ${ns[ns.length - 1].toFixed(0)} ns`);
        console.log('  前 12 个窄脉冲（上升沿时刻 ms / 低 ns / 高 ns / 之前 SIg）:');
        for (const p of seedLike.slice(0, 12))
            console.log(`    ${(p.rise * 1e3).toFixed(4)} ms  低 ${p.lowNs.toFixed(0)} ns  高 ${p.highNs.toFixed(0)} ns  SIg=${p.sig}`);
    }
    const hn = normal.map((p) => p.highNs).sort((a, b) => a - b);
    if (hn.length) console.log(`  宽脉冲高电平: min ${hn[0].toFixed(0)} ns  中位 ${hn[(hn.length / 2) | 0].toFixed(0)} ns  max ${hn[hn.length - 1].toFixed(0)} ns`);
    console.log(`  LAT 低脉冲 ${latEdges.length} 个；其中 CLKg 同时为低/高的?（LAT 低沿时 CLKg=${latEdges.filter((e) => e.clkg === 0).length} 个为低）`);
})();
