/*
 * byte_offset.js —— 逐字节找"tick 位流相对 pio 位流"的最佳偏移，识别非均匀错位
 * 用法：node tests/tools/byte_offset.js <pio.csv> <tick.csv> [扫描号]
 */
'use strict';
const fs = require('fs');
const readline = require('readline');
const SCANS = 43, BITS = 384;

async function frameBits(file) {
    const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
    let first = true, prev = null, scanClocks = 0, scanPulses = 0, bitBuf = [], cur = [], last = null;
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
                if (cur.length < SCANS) cur.push(bitBuf.slice());
                if (cur.length === SCANS) last = cur;
                scanClocks = 0; scanPulses = 0; bitBuf = [];
            }
        }
        prev = v;
    }
    return last.map((b) => b.join(''));
}

(async () => {
    const [pf, tf] = [process.argv[2], process.argv[3]];
    const scan = parseInt(process.argv[4] || '0', 10);
    const P = await frameBits(pf), T = await frameBits(tf);
    const p = P[scan], t = T[scan];
    console.log(`扫描 ${scan}：pio ${p.length} 位，tick ${t.length} 位`);
    console.log('逐字节最佳偏移（把 pio 的第 j 字节与 tick 的 [j*8+o, +8) 比较）:');
    const hist = new Map();
    let rows = [];
    for (let j = 0; j < 48; ++j) {
        let best = { o: 0, r: -1 };
        for (let o = -8; o <= 8; ++o) {
            let same = 0, tot = 0;
            for (let k = 0; k < 8; ++k) {
                const ti = j * 8 + o + k;
                if (ti < 0 || ti >= t.length) continue;
                tot++; if (p[j * 8 + k] === t[ti]) same++;
            }
            const r = tot ? same / tot : 0;
            if (r > best.r) best = { o, r };
        }
        hist.set(best.o, (hist.get(best.o) || 0) + 1);
        rows.push(`  byte ${String(j).padStart(2)}: 最佳 o=${String(best.o).padStart(3)}  一致 ${(best.r * 100).toFixed(0)}%`);
    }
    rows.slice(0, 16).forEach((r) => console.log(r));
    console.log('  …（只列前 16 个）');
    console.log('偏移直方图: ' + [...hist.entries()].sort((a, b) => a[0] - b[0]).map(([k, n]) => `${k}:${n}`).join('  '));

    /* 逐扫描的"整帧最佳偏移"，看偏移是否随扫描号递增（= 丢位累积） */
    console.log('\n逐扫描整帧最佳偏移（看是否随扫描号线性增长 ⇒ 每字节丢/多位）:');
    for (let s = 0; s < SCANS; s += 6) {
        let best = { o: 0, r: -1 };
        for (let o = -12; o <= 12; ++o) {
            let same = 0, tot = 0;
            for (let i = 0; i < BITS; ++i) {
                const ti = i + o;
                if (ti < 0 || ti >= T[s].length) continue;
                tot++; if (P[s][i] === T[s][ti]) same++;
            }
            const r = tot ? same / tot : 0;
            if (r > best.r) best = { o, r };
        }
        console.log(`  scan ${String(s).padStart(2)}: 最佳 o=${String(best.o).padStart(3)} 位  一致 ${(best.r * 100).toFixed(1)}%`);
    }
})();
