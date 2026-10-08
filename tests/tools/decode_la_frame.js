/*
 * decode_la_frame.js —— 从逻辑分析仪抓包里**解码 MCU 实际发出的 43×48 字节**
 *
 * 思路：SPI 是 LSB-first，CLKa 上升沿采样 SIa。抓包是"边沿压缩"导出（电平在两次变化之间保持），
 *       所以只要记录每一行各通道的电平，就能在 CLKa 上升沿处取到 SIa 的真实值。
 *
 * 输出：
 *   · 每个扫描周期（以 BK 上升沿 = 消隐起点划分）的 CLKa 时钟数（期望 384）
 *   · 每扫描的 CLKg 上升沿数（期望 1；帧首那一次是 6）与 SIg 在各次上升沿的取值
 *   · 解码出的 43×48 字节帧（hex），可写出二进制文件供宿主机逐字节对比
 *
 * 用法：node tests/tools/decode_la_frame.js <csv> [输出前缀]
 */
'use strict';
const fs = require('fs');
const readline = require('readline');

const file = process.argv[2];
const prefix = process.argv[3] || null;

(async () => {
    const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
    let hdr = null, prev = null, prevT = null;
    const dtHist = new Map();

    let lastSia = 0, lastClka = 0, lastClkg = 1, lastLat = 1, lastBk = 1, lastSig = 0;
    const clkRise = [];          /* { t, sia, clkg, lat, bk } */
    const clkgRise = [];         /* { t, sig, lat, bk } */
    const bkRise = [];           /* { t } 消隐开始 = 扫描起点 */
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
            const k = Math.round((t - prevT) * 1e9 / 20) * 20;
            dtHist.set(k, (dtHist.get(k) || 0) + 1);
        }
        /* 边沿判定（用上一行的电平，符合"变化点"语义） */
        if (prev) {
            if (prev.CLKa === 0 && v.CLKa === 1) clkRise.push({ t, sia: prev.SIa, clkg: prev.CLKg, lat: prev.LAT, bk: prev.BK });
            if (prev.CLKg === 0 && v.CLKg === 1) clkgRise.push({ t, sig: prev.SIg, lat: prev.LAT, bk: prev.BK });
            if (prev.BK === 0 && v.BK === 1) bkRise.push({ t });
        }
        prev = v; prevT = t;
        lastSia = v.SIa; lastClka = v.CLKa; lastClkg = v.CLKg; lastLat = v.LAT; lastBk = v.BK; lastSig = v.SIg;
    }

    const top = [...dtHist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 5);
    console.log(`文件 ${file}`);
    console.log(`行数 ${rows}  时间跨度 ${((last - first) * 1e3).toFixed(2)} ms  时间步长 Top5: ` +
        top.map(([k, n]) => `${k}ns×${n}`).join(', '));
    console.log(`CLKa 上升沿 ${clkRise.length} 个 · CLKg 上升沿 ${clkgRise.length} 个 · BK 上升沿 ${bkRise.length} 个（≈ 扫描数）`);
    console.log(`每扫描平均 CLKa 时钟 = ${(clkRise.length / Math.max(1, bkRise.length)).toFixed(1)}（期望 384）`);

    /* 以 BK 上升沿划分扫描，收集每个扫描内的 CLKa 时钟与 CLKg 脉冲 */
    const scans = [];
    let cur = null;
    let ci = 0;
    for (const bk of bkRise) {
        if (cur) { cur.clocks = clkRise.slice(cur.i0, ci); cur.end = bk.t; scans.push(cur); }
        cur = { start: bk.t, i0: ci, clocks: [], clkg: [] };
        /* 该扫描内的 CLKg 脉冲 */
        while (ci < clkRise.length && clkRise[ci].t < bk.t) ci++;   /* 对齐时钟索引 */
        const nextBk = bkRise[bkRise.indexOf(bk) + 1];
        cur.i0 = ci;
        cur.clkg = clkgRise.filter((e) => e.t >= bk.t && (!nextBk || e.t < nextBk.t));
        /* 把 ci 推到该扫描结束（下一个 BK）之前 */
        let j = ci;
        while (j < clkRise.length && (!nextBk || clkRise[j].t < nextBk.t)) j++;
        cur.clocks = clkRise.slice(ci, j);
        ci = j;
        scans.push(cur);
        cur = null;
    }

    const sizes = new Map();
    for (const s of scans) sizes.set(s.clocks.length, (sizes.get(s.clocks.length) || 0) + 1);
    console.log('每扫描 CLKa 时钟数分布: ' + [...sizes.entries()].sort((a, b) => b[1] - a[1]).slice(0, 10)
        .map(([k, n]) => `${k}个×${n}`).join(', '));

    const clkgCount = new Map();
    for (const s of scans) clkgCount.set(s.clkg.length, (clkgCount.get(s.clkg.length) || 0) + 1);
    console.log('每扫描 CLKg 上升沿数分布: ' + [...clkgCount.entries()].sort((a, b) => a[0] - b[0])
        .map(([k, n]) => `${k}个×${n}`).join(', '));

    /* 完整扫描（384 时钟）→ 组装 48 字节（LSB-first） */
    const full = scans.filter((s) => s.clocks.length === 384);
    console.log(`完整扫描 ${full.length} 个（用于解码）`);
    const frames = [];
    for (let i = 0; i + 43 <= full.length; i += 43) frames.push(full.slice(i, i + 43));
    console.log(`可拼出完整帧 ${frames.length} 个`);

    if (frames.length) {
        const f = frames[frames.length - 1];              /* 取最后一帧（前面可能被截断） */
        const bytes = [];
        for (const s of f) {
            for (let b = 0; b < 48; ++b) {
                let v = 0;
                for (let k = 0; k < 8; ++k) v |= (s.clocks[b * 8 + k].sia & 1) << k;
                bytes.push(v);
            }
        }
        const hex = bytes.map((v) => v.toString(16).padStart(2, '0')).join('');
        console.log(`解码帧（扫描 0 的前 24 字节）: ${bytes.slice(0, 24).map((v) => v.toString(16).padStart(2, '0')).join(' ')}`);
        if (prefix) {
            fs.writeFileSync(prefix + '_captured.bin', Buffer.from(bytes));
            fs.writeFileSync(prefix + '_captured.hex', hex + '\n');
            console.log(`已写出 ${prefix}_captured.bin（${bytes.length} 字节）与 _captured.hex`);
        }
        /* 帧首与帧尾各扫描的 CLKg 结构 */
        console.log('\n该帧各扫描的 CLKg 上升沿数 / SIg 序列:');
        f.forEach((s, idx) => {
            const sigs = s.clkg.map((e) => e.sig).join('');
            console.log(`  scan ${String(idx).padStart(2)}: ${s.clkg.length} 个  SIg=${sigs}`);
        });
    }
})();
