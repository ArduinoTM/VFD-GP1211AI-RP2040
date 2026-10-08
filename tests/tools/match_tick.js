/*
 * match_tick.js —— 用"pio 已验证的期望帧"当基准，穷举变换，找出 tick 实际在做的事
 *
 * 基准：pio 抓包解码出的 43×48 字节（LSB-first 到达顺序）——它与自检画面逐点 100% 一致，
 *       所以它就是"逻辑上正确的位流"。
 * 目标：找出变换 f，使 f(基准) 与 tick 抓包解码出的位流 **逐位一致**。
 *
 * 用法：node tests/tools/match_tick.js <pio.csv> <tick.csv>
 */
'use strict';
const fs = require('fs');
const readline = require('readline');
const SCANS = 43, BYTES = 48, BITS = BYTES * 8, FBITS = SCANS * BITS;

async function frameBytes(file) {
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
    /* 转成"每扫描 48 字节，bit i = 到达顺序第 i 位"（LSB-first 组装） */
    return last.map((bits) => {
        const out = new Uint8Array(BYTES);
        for (let i = 0; i < BITS; ++i) if (bits[i]) out[i >> 3] |= 1 << (i & 7);
        return out;
    });
}

const rv8 = (b) => { b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4); b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2); b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1); return b & 0xFF; };
const nib = (b) => ((b << 4) | (b >> 4)) & 0xFF;

function applyScan(scan, mode, shift) {
    let b = Uint8Array.from(scan);
    if (mode === 'revBits') b = b.map(rv8);
    else if (mode === 'nib') b = b.map(nib);
    /* 每扫描 384 位（LSB-first）整体循环左移 shift */
    const n = ((shift % BITS) + BITS) % BITS;
    if (n) {
        const getBit = (i) => (b[(i >> 3) % BYTES] >> ((i % BITS) & 7)) & 1;
        const out = new Uint8Array(BYTES);
        for (let i = 0; i < BITS; ++i) if (getBit((i + n) % BITS)) out[i >> 3] |= 1 << (i & 7);
        b = out;
    }
    return b;
}

function agreement(L, T, mode, shift, scanRot) {
    let same = 0, tot = 0;
    for (let s = 0; s < SCANS; ++s) {
        const src = L[(s + scanRot + SCANS) % SCANS];
        const a = applyScan(src, mode, shift);
        const b = T[s];
        for (let i = 0; i < BYTES; ++i) {
            let x = a[i], y = b[i];
            for (let k = 0; k < 8; ++k) { tot++; if (((x >> k) & 1) === ((y >> k) & 1)) same++; }
        }
    }
    return same / tot;
}

(async () => {
    const L = await frameBytes(process.argv[2]);
    const T = await frameBytes(process.argv[3]);
    console.log('基准(pio) 与 待解(tick) 各解出 %d 个扫描 × %d 字节', L.length, T.length);

    const modes = ['none', 'revBits', 'nib'];
    const results = [];
    for (const mode of modes) for (let sh = -8; sh <= 8; ++sh) for (let rot = -1; rot <= 1; ++rot)
        results.push({ mode, sh, rot, r: agreement(L, T, mode, sh, rot) });
    results.sort((a, b) => b.r - a.r);
    console.log('\n全部候选里最好的 8 个:');
    for (const r of results.slice(0, 8))
        console.log(`  mode=${r.mode.padEnd(7)} 位移=${String(r.sh).padStart(3)} 扫描旋转=${String(r.rot).padStart(2)} ⇒ 逐位一致率 ${(r.r * 100).toFixed(2)}%`);

    const best = results[0];
    console.log(`\n最佳: mode=${best.mode} 位移=${best.sh} 扫描旋转=${best.rot} ⇒ ${(best.r * 100).toFixed(2)}%`);
    console.log('逐扫描一致率（最佳候选）:');
    for (let s = 0; s < SCANS; ++s) {
        const src = L[(s + best.rot + SCANS) % SCANS];
        const a = applyScan(src, best.mode, best.sh);
        const b = T[s];
        let same = 0;
        for (let i = 0; i < BYTES; ++i) for (let k = 0; k < 8; ++k) if (((a[i] >> k) & 1) === ((b[i] >> k) & 1)) same++;
        process.stdout.write(`${s}:${(100 * same / BITS).toFixed(0)}%  `);
    }
    console.log('\n\n逐扫描各自的最佳候选（看是否一致 ⇒ 单一变换即可解释）:');
    for (let s = 0; s < SCANS; s += 6) {
        let bb = { r: -1 };
        for (const mode of modes) for (let sh = -8; sh <= 8; ++sh) for (let rot = -1; rot <= 1; ++rot) {
            const src = L[(s + rot + SCANS) % SCANS];
            const a = applyScan(src, mode, sh), b = T[s];
            let same = 0;
            for (let i = 0; i < BYTES; ++i) for (let k = 0; k < 8; ++k) if (((a[i] >> k) & 1) === ((b[i] >> k) & 1)) same++;
            const r = same / BITS;
            if (r > bb.r) bb = { r, mode, sh, rot };
        }
        console.log(`  scan ${String(s).padStart(2)}: mode=${bb.mode.padEnd(7)} 位移=${String(bb.sh).padStart(3)} 旋转=${bb.rot} ⇒ ${(bb.r * 100).toFixed(1)}%`);
    }
})();
