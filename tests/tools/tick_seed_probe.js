/*
 * tick_seed_probe.js —— 在 tick 版抓包里逐帧看"帧首那 6 个 CLKg 脉冲"的真实结构
 *
 * 关注点（显示异常时最可疑的地方）：
 *   · 帧首组到底是几个脉冲、彼此间隔多少（正常：1 个前进 + 5 个播种，每个约 1 µs 低 + 高）
 *   · 每个 CLKg 上升沿处 SIg 的取值（正常：0,1,1,0,0,0）
 *   · LAT 相对 CLKg 的位置、BK 状态
 *   · 时间步长基准（判断抓包分辨率，决定上述结论可信度）
 *
 * 用法：node tests/tools/tick_seed_probe.js <csv> [打印几组]
 */
'use strict';
const fs = require('fs');
const readline = require('readline');

const file = process.argv[2];
const maxGroups = parseInt(process.argv[3] || '6', 10);

(async () => {
    const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
    let hdr = null, prev = null, prevT = null;
    const dtHist = new Map();
    const clkEdges = [];        // { t, to, sig, lat, bk }
    const latEdges = [];        // { t, to, clkg, bk }
    let rows = 0, first = null, last = null;

    for await (const line of rl) {
        if (!line) continue;
        const p = line.split(',');
        if (!hdr) { hdr = p.map((s) => s.trim()); continue; }
        const t = parseFloat(p[0]);
        const v = { CLKa: +p[1], SIa: +p[2], LAT: +p[3], CLKg: +p[4], SIg: +p[5], BK: +p[6] };
        rows++;
        if (first === null) first = t;
        last = t;
        if (prevT !== null) {
            const dt = t - prevT;
            const key = Math.round(dt * 1e9 / 20) * 20;   // 以 20 ns 为量化单位
            dtHist.set(key, (dtHist.get(key) || 0) + 1);
        }
        if (prev) {
            if (v.CLKg !== prev.CLKg) clkEdges.push({ t, to: v.CLKg, sig: prev.SIg, lat: prev.LAT, bk: prev.BK });
            if (v.LAT !== prev.LAT) latEdges.push({ t, to: v.LAT, clkg: v.CLKg, bk: v.BK, sig: v.SIg });
        }
        prev = v; prevT = t;
    }

    const top = [...dtHist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8);
    console.log(`文件: ${file}`);
    console.log(`样本 ${rows} 行，时间跨度 ${((last - first) * 1e3).toFixed(2)} ms`);
    console.log('时间步长（量化到 20 ns）分布 Top8: ' + top.map(([k, n]) => `${k}ns×${n}`).join(', '));
    const nonMult = [...dtHist.keys()].filter((k) => k % 20 !== 0).length;
    console.log(`步长非 20 ns 整数倍的种类数: ${nonMult}（0 ⇒ 时间基准就是 20 ns = 50 MHz）`);

    // 帧首组：上升沿间隔 < 20 µs 归为一组，取脉冲数 >= 4 的组
    const groups = [];
    for (const e of clkEdges) {
        if (e.to !== 1) continue;
        if (!groups.length || e.t - groups[groups.length - 1].last > 20e-6) groups.push({ first: e.t, last: e.t, edges: [e] });
        else { const g = groups[groups.length - 1]; g.last = e.t; g.edges.push(e); }
    }
    const sizes = new Map();
    for (const g of groups) sizes.set(g.edges.length, (sizes.get(g.edges.length) || 0) + 1);
    console.log('CLKg 脉冲组大小分布: ' + [...sizes.entries()].sort((a, b) => a[0] - b[0]).map(([k, n]) => `${k}连发×${n}`).join(', '));

    const big = groups.filter((g) => g.edges.length >= 4);
    console.log(`\n脉冲数 >= 4 的组共 ${big.length} 个，前 ${Math.min(maxGroups, big.length)} 个的细节：`);
    for (const g of big.slice(0, maxGroups)) {
        const rel = g.edges.map((e) => ((e.t - g.first) * 1e6).toFixed(2));
        const sig = g.edges.map((e) => e.sig).join('');
        const lat = g.edges.map((e) => e.lat).join('');
        console.log(`  t=${(g.first * 1e3).toFixed(4)} ms  脉冲数=${g.edges.length}  SIg=${sig}  LAT=${lat}`);
        console.log(`    相对首个上升沿(µs): ${rel.join(', ')}`);
        // 每个上升沿到下一个上升沿的间隔
        const gaps = [];
        for (let i = 1; i < g.edges.length; ++i) gaps.push(((g.edges[i].t - g.edges[i - 1].t) * 1e6).toFixed(2));
        console.log(`    脉冲间隔(µs): ${gaps.join(', ')}`);
        console.log(`    平均间隔 ${(gaps.reduce((a, b) => a + parseFloat(b), 0) / gaps.length).toFixed(2)} µs（正常约 1.9 µs）`);
    }

    // 单脉冲组里 SIg 是否为高（正常应为 0）
    const singles = groups.filter((g) => g.edges.length === 1);
    const badSingle = singles.filter((g) => g.edges[0].sig === 1);
    console.log(`\n单脉冲组 ${singles.length} 个，其中上升沿处 SIg=1 的 ${badSingle.length} 个（正常 0）`);
    // SIg 高电平段统计
    const sigHigh = [];
    let inHigh = null; const segs = [];
    prev = null;
    const rl2 = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
    let firstLine = true;
    for await (const line of rl2) {
        if (!line) continue;
        const p = line.split(',');
        if (firstLine) { firstLine = false; continue; }
        const t = parseFloat(p[0]); const s = +p[5];
        if (prev !== null && s !== prev) { if (s === 1) inHigh = t; else if (inHigh !== null) { segs.push(t - inHigh); inHigh = null; } }
        prev = s;
    }
    const w = segs.map((x) => x * 1e6);
    w.sort((a, b) => a - b);
    console.log(`SIg 高电平段 ${w.length} 段：最小 ${w[0] ? w[0].toFixed(2) : '-'} µs / 中位 ${w.length ? w[(w.length / 2) | 0].toFixed(2) : '-'} µs / 最大 ${w.length ? w[w.length - 1].toFixed(2) : '-'} µs`);
    // LAT 低脉冲间隔
    const latLow = latEdges.filter((e) => e.to === 0);
    const iv = [];
    for (let i = 1; i < latLow.length; ++i) iv.push((latLow[i].t - latLow[i - 1].t) * 1e6);
    iv.sort((a, b) => a - b);
    console.log(`LAT 低脉冲 ${latLow.length} 个，间隔 最小 ${iv[0] ? iv[0].toFixed(2) : '-'} µs / 中位 ${iv.length ? iv[(iv.length / 2) | 0].toFixed(2) : '-'} µs / 最大 ${iv.length ? iv[iv.length - 1].toFixed(2) : '-'} µs（正常 189 µs）`);
})();
