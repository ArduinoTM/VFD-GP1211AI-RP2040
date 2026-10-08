/*
 * check_la_vs_expected.js —— 抓包解码结果 vs 宿主机期望字节：逐扫描比对 + 位对齐搜索
 *
 * 用法：node tests/tools/check_la_vs_expected.js <csv> <expected.bin>
 */
'use strict';
const fs = require('fs');
const readline = require('readline');

const csv = process.argv[2];
const expFile = process.argv[3];
const exp = fs.readFileSync(expFile);

(async () => {
    const rl = readline.createInterface({ input: fs.createReadStream(csv), crlfDelay: Infinity });
    let hdr = null, prev = null, prevT = null;
    const clkRise = [], clkgRise = [], bkRise = [];
    const ioEvents = [];
    let first = null, last = null, rows = 0;

    for await (const line of rl) {
        if (!line) continue;
        const p = line.split(',');
        if (!hdr) { hdr = p.map((s) => s.trim()); continue; }
        const t = parseFloat(p[0]);
        const v = { CLKa: +p[1], SIa: +p[2], LAT: +p[3], CLKg: +p[4], SIg: +p[5], BK: +p[6], HVEN: +p[7], FLEN: +p[8] };
        rows++; if (first === null) first = t; last = t;
        if (prev) {
            if (prev.CLKa === 0 && v.CLKa === 1) clkRise.push({ t, sia: v.SIa });
            if (prev.CLKg === 0 && v.CLKg === 1) clkgRise.push({ t, sig: prev.SIg });
            if (prev.BK === 0 && v.BK === 1) bkRise.push({ t });
            if (v.HVEN !== prev.HVEN) ioEvents.push({ t, name: 'HVEN', to: v.HVEN });
            if (v.FLEN !== prev.FLEN) ioEvents.push({ t, name: 'FLEN', to: v.FLEN });
            if (v.BK !== prev.BK && ioEvents.length < 20) { /* BK 边沿太多，只记录前几个供参考 */ }
        }
        prev = v; prevT = t;
    }

    console.log(`文件 ${csv}\n行数 ${rows}  时间跨度 ${((last - first) * 1e3).toFixed(1)} ms`);
    console.log('上电/使能事件: ' + ioEvents.slice(0, 8).map((e) => `${(e.t * 1e3).toFixed(2)}ms ${e.name}=${e.to}`).join('  '));

    /* 扫描切分 */
    const scans = [];
    for (let i = 0; i < bkRise.length; ++i) {
        const t0 = bkRise[i].t, t1 = (i + 1 < bkRise.length) ? bkRise[i + 1].t : Infinity;
        scans.push({ t0, clocks: clkRise.filter((e) => e.t >= t0 && e.t < t1), pulses: clkgRise.filter((e) => e.t >= t0 && e.t < t1) });
    }
    const withData = scans.filter((s) => s.clocks.length === 384);
    console.log(`扫描 ${scans.length} 个；其中完整 384 时钟 ${withData.length} 个，0 时钟 ${scans.filter((s) => s.clocks.length === 0).length} 个`);

    const frameStarts = [];
    scans.forEach((s, i) => { if (s.pulses.length === 6) frameStarts.push(i); });
    console.log(`帧首（6 连发）${frameStarts.length} 个；SIg 序列 = ${[...new Set(frameStarts.map((i) => scans[i].pulses.map((p) => p.sig).join('')))].join(' / ')}`);

    /* 取最后一个完整 43 扫描的帧 */
    let f0 = -1, f1 = -1;
    for (let k = frameStarts.length - 1; k >= 1; --k) {
        if (frameStarts[k] - frameStarts[k - 1] === 43) { f0 = frameStarts[k - 1]; f1 = frameStarts[k]; break; }
    }
    if (f0 < 0) { console.log('没找到长度正好 43 的帧'); return; }
    console.log(`\n对比帧：扫描 ${f0}..${f1 - 1}（43 个扫描）  t = ${(scans[f0].t0 * 1e3).toFixed(1)} ms`);

    /* 解码该帧的位流（43×384 bit，按扫描顺序） */
    const frame = scans.slice(f0, f1);
    let bits = '';
    let missing = 0;
    for (const s of frame) {
        if (s.clocks.length !== 384) { missing++; bits += '0'.repeat(384); continue; }
        for (const c of s.clocks) bits += (c.sia & 1) ? '1' : '0';
    }
    console.log(`位流长度 ${bits.length}（期望 16512）；缺数据扫描 ${missing} 个`);

    const expBits = [];
    for (let i = 0; i < exp.length; ++i) for (let b = 0; b < 8; ++b) expBits.push((exp[i] >> b) & 1);
    const expStr = expBits.join('');

    /* 位对齐搜索：把我的位流整体平移 k 位后与期望比较 */
    console.log('\n位对齐搜索（整体平移 k 位后的匹配率）:');
    const N = 16512;
    let best = { k: 0, rate: -1 };
    for (let k = -12; k <= 12; ++k) {
        let same = 0, tot = 0;
        for (let i = 0; i < N; ++i) {
            const j = i + k;
            if (j < 0 || j >= bits.length) continue;
            tot++; if (bits[j] === expStr[i]) same++;
        }
        const rate = tot ? same / tot : 0;
        if (rate > best.rate) best = { k, rate };
        if (k % 3 === 0 || rate > 0.95) console.log(`  k=${String(k).padStart(3)} 位: ${(rate * 100).toFixed(2)}%`);
    }
    console.log(`⇒ 最佳 k=${best.k} 位，匹配率 ${(best.rate * 100).toFixed(2)}%`);

    /* 扫描旋转搜索：面板在扫描边界的 LAT 锁存到的是**上一扫描**移入的数据（边界在本扫描
     * 移位之前）⇒ 现版固件（scanPhase = −1）在总线上是"提前一个扫描"发数据：
     * 抓包里第 s 个扫描 == 期望逻辑帧的第 s+1 个扫描 ⇒ 期望的最佳旋转 r = −1（等价 +42）。
     * （位对齐搜索只能看 ±12 位，一个扫描旋转 = 384 位，必须单独搜。） */
    console.log('\n扫描旋转搜索（期望整体旋转 r 个扫描后逐字节比较；正 = 抓包 s 对应期望 s−r）:');
    let bestR = { r: 0, rate: -1 };
    for (let r = -3; r <= 3; ++r) {
        let same = 0, tot = 0;
        for (let s = 0; s < 43; ++s) {
            const sc = frame[s];
            if (sc.clocks.length !== 384) continue;
            const es = ((s - r) % 43 + 43) % 43;
            for (let b = 0; b < 48; ++b) {
                let v = 0;
                for (let k = 0; k < 8; ++k) v |= (sc.clocks[b * 8 + k].sia & 1) << k;
                tot++;
                if (v === exp[es * 48 + b]) same++;
            }
        }
        const rate = tot ? same / tot : 0;
        if (rate > bestR.rate) bestR = { r, rate };
        console.log(`  r=${String(r).padStart(2)} 扫描: ${(rate * 100).toFixed(2)}%`);
    }
    console.log(`⇒ 最佳扫描旋转 r=${bestR.r}（匹配率 ${(bestR.rate * 100).toFixed(2)}%）` +
        (bestR.rate === 1
            ? (bestR.r === -1
                ? '  ✔ 与"面板锁存上一扫描 + scanPhase=−1"一致（现版固件应当如此）'
                : '  ⚠️ 完全匹配但旋转不是 −1：帧数据应经 wirePrepareFrame()（相位固定在 −1）')
            : '  ⚠️ 没有完全匹配的旋转：数据内容本身有问题（先看上面的位对齐搜索）'));

    /* 逐扫描字节匹配（原样） */
    let ok = 0, bad = 0;
    for (let s = 0; s < 43; ++s) {
        const sc = frame[s];
        if (sc.clocks.length !== 384) { bad++; continue; }
        let diff = 0;
        for (let b = 0; b < 48; ++b) {
            let v = 0;
            for (let k = 0; k < 8; ++k) v |= (sc.clocks[b * 8 + k].sia & 1) << k;
            if (v !== exp[s * 48 + b]) diff++;
        }
        if (diff === 0) ok++; else bad++;
    }
    console.log(`逐扫描（原样对齐）：完全一致 ${ok} 个扫描，有差异 ${bad} 个`);
})();
