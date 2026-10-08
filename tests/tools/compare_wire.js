/*
 * compare_wire.js —— 把两份抓包各自解码成"总线位流"，逐位对比（pio = 参考，tick = 待诊断）
 *
 * 用法：node tests/tools/compare_wire.js <pio.csv> <tick.csv>
 *
 * 输出：
 *   · 两份文件各自的帧结构（扫描数、帧首 6 连发的 SIg 序列、每扫描时钟数）
 *   · 最后一整帧的位流（43×384 bit，按"先到的位在前"排列，即 LSB-first 组装）
 *   · 逐位相同率；若不同，再判定差异形态：逐字节位反序 / 整体位移 / 扫描旋转 / 其它
 */
'use strict';
const fs = require('fs');
const readline = require('readline');

const SCANS = 43, BITS = 384, BYTES = 48, FRAME_BITS = SCANS * BITS;

function reverseByte(b) {
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b & 0xFF;
}

/* 解码一份抓包：返回最后一整帧的位串（字符串，'0'/'1'，长度 FRAME_BITS）以及统计 */
async function decodeFrame(file) {
    const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
    let hdr = null, prev = null, first = true;
    const stats = { rows: 0, t0: null, t1: null, clk: 0, clkg: 0, bk: 0, sizes: new Map(), group: new Map() };
    let scanClocks = 0;                 /* 当前扫描内 CLKa 上升沿数 */
    let scanPulses = 0;                 /* 当前扫描内 CLKg 上升沿数 */
    let scanSig = '';                   /* 当前扫描内各 CLKg 上升沿处的 SIg 值 */
    let bitBuf = [];                    /* 当前扫描的位（按到达顺序） */
    let cur = [];                       /* 当前帧已收集的扫描 */
    let lastFrame = null;

    for await (const line of rl) {
        if (!line) continue;
        const p = line.split(',');
        if (first) { first = false; hdr = p.map((s) => s.trim()); continue; }
        const t = parseFloat(p[0]);
        const v = { CLKa: +p[1], SIa: +p[2], LAT: +p[3], CLKg: +p[4], SIg: +p[5], BK: +p[6] };
        stats.rows++;
        if (stats.t0 === null) stats.t0 = t;
        stats.t1 = t;
        if (prev) {
            if (prev.CLKa === 0 && v.CLKa === 1) { stats.clk++; scanClocks++; bitBuf.push(v.SIa & 1); }
            if (prev.CLKg === 0 && v.CLKg === 1) { stats.clkg++; scanPulses++; scanSig += (prev.SIg & 1); }
            if (prev.BK === 0 && v.BK === 1) {
                /* 一个扫描结束：归档 */
                stats.bk++;
                if (scanClocks) stats.sizes.set(scanClocks, (stats.sizes.get(scanClocks) || 0) + 1);
                if (scanPulses) stats.group.set(scanPulses, (stats.group.get(scanPulses) || 0) + 1);
                if (scanPulses === 6) {
                    /* 帧首：开始收集新的一帧 */
                    cur = [];
                }
                if (cur.length < SCANS) cur.push({ n: scanClocks, bits: bitBuf.slice(), sig: scanSig });
                if (cur.length === SCANS) lastFrame = cur.map((s) => s);
                scanClocks = 0; scanPulses = 0; scanSig = ''; bitBuf = [];
            }
        }
        prev = v;
    }
    let bits = '';
    if (lastFrame) for (const s of lastFrame) {
        if (s.bits.length === BITS) bits += s.bits.join('');
        else bits += '0'.repeat(BITS);
    }
    return { stats, bits, frame: lastFrame };
}

function hamming(a, b) {
    if (a.length !== b.length) return -1;
    let same = 0;
    for (let i = 0; i < a.length; ++i) if (a[i] === b[i]) same++;
    return same;
}

(async () => {
    const [pf, tf] = [process.argv[2], process.argv[3]];
    for (const f of [pf, tf]) {
        const r = await decodeFrame(f);
        const topSizes = [...r.stats.sizes.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4);
        const topGroups = [...r.stats.group.entries()].sort((a, b) => a[0] - b[0]).slice(0, 6);
        console.log(`\n=== ${f.split(/[\\/]/).pop()} ===`);
        console.log(`  行数 ${r.stats.rows}  跨度 ${((r.stats.t1 - r.stats.t0) * 1e3).toFixed(1)} ms`);
        console.log(`  CLKa 上升沿 ${r.stats.clk} · CLKg ${r.stats.clkg} · BK(${r.stats.bk} 个扫描)`);
        console.log(`  每扫描时钟数分布: ${topSizes.map(([k, n]) => `${k}×${n}`).join(', ')}`);
        console.log(`  每扫描 CLKg 脉冲数分布: ${topGroups.map(([k, n]) => `${k}个×${n}`).join(', ')}`);
        console.log(`  最后一整帧位流长度 ${r.bits.length}（期望 ${FRAME_BITS}）`);
    }

    const P = await decodeFrame(pf);
    const T = await decodeFrame(tf);
    if (!P.bits.length || !T.bits.length) { console.log('\n有一份没解出完整帧，先看上面的统计'); return; }

    const same = hamming(P.bits, T.bits);
    console.log(`\n=== 位流对比（pio 为参考）===`);
    console.log(`  逐位相同率: ${(100 * same / FRAME_BITS).toFixed(2)}%`);

    /* 差异形态判定 */
    const flip = (s) => {           /* 逐字节位反序 */
        let out = '';
        for (let b = 0; b < BYTES * SCANS; ++b) {
            let v = 0;
            for (let k = 0; k < 8; ++k) v |= (s[b * 8 + k] === '1' ? 1 : 0) << k;
            const r = reverseByte(v);
            for (let k = 0; k < 8; ++k) out += ((r >> k) & 1) ? '1' : '0';
        }
        return out;
    };
    const rot = (s, n) => s.slice(n) + s.slice(0, n);   /* 整体位移 n 位 */

    console.log(`  若把 tick 逐字节位反序后: ${(100 * hamming(P.bits, flip(T.bits)) / FRAME_BITS).toFixed(2)}%`);
    let best = { n: 0, r: -1 };
    for (let n = -16; n <= 16; ++n) {
        const cand = n >= 0 ? rot(T.bits, n) : rot(T.bits, FRAME_BITS + n);
        const r = hamming(P.bits, cand) / FRAME_BITS;
        if (r > best.r) best = { n, r };
    }
    console.log(`  整体位移最佳 n=${best.n} 位 ⇒ ${(100 * best.r).toFixed(2)}%`);

    /* 逐扫描 48 字节级别比较：看是"整帧都不同"还是"个别扫描不同" */
    let scanSame = 0, scanDiff = 0;
    for (let s = 0; s < SCANS; ++s) {
        const a = P.bits.slice(s * BITS, (s + 1) * BITS);
        const b = T.bits.slice(s * BITS, (s + 1) * BITS);
        const eq = hamming(a, b) === BITS;
        if (eq) scanSame++; else scanDiff++;
    }
    console.log(`  逐扫描完全一致: ${scanSame}/${SCANS}（不一致 ${scanDiff}）`);

    /* 打印第一个不一致扫描的两个 48 字节（hex）便于肉眼看 */
    for (let s = 0; s < SCANS; ++s) {
        const a = P.bits.slice(s * BITS, (s + 1) * BITS);
        const b = T.bits.slice(s * BITS, (s + 1) * BITS);
        if (hamming(a, b) === BITS) continue;
        const hex = (bits) => {
            let out = [];
            for (let i = 0; i < BYTES; ++i) {
                let v = 0;
                for (let k = 0; k < 8; ++k) v |= (bits[i * 8 + k] === '1' ? 1 : 0) << k;
                out.push(v.toString(16).padStart(2, '0'));
            }
            return out.join(' ');
        };
        console.log(`\n  第一个不一致扫描 #${s}:`);
        console.log(`    pio : ${hex(a).slice(0, 96)}...`);
        console.log(`    tick: ${hex(b).slice(0, 96)}...`);
        break;
    }
})();
