#!/usr/bin/env node
/*
 * check_seed_window.js —— 用逻辑分析仪抓包核对**现行**帧末播种的落点（真机可核对判据）
 *
 * 背景：2026-10-08 起 PIO 帧首播种由 CPU 在整帧 DMA 完成中断里完成（先自旋等 SM 停进 wait，
 * 再翻 CLKg/SIg 引脚，见 src/vfd_platform_rp2040_pio.cpp 的 onFrameDmaDone()）。
 * 那套"请求脚 + jmp pin"的旧握手已删除；本脚本给现行机制补一条**可从波形直接判定**的判据：
 *
 *   A 播种组（帧首那段 SIg 高电平 + 其间的 CLKg 脉冲）**不得被 BK 边沿打断**
 *     —— 否则就是在点亮窗口里推进栅极链（画面会闪/错位）；
 *   B 播种组期间 **LAT 始终为低** —— 否则这次链搬运会被锁存到屏幕上（可见的错位）；
 *   C 播种组结束后到**下一个 LAT 上升沿（帧首扫描的锁存=边界③）**必须留正余量，并打印该余量；
 *   D 每组至少有 1 个 CLKg 上升沿（真的在移位），且抓包里至少出现 1 组（测不到 = 不通过）。
 *
 * 用法：node tests/tools/check_seed_window.js <capture.csv[.gz]>
 * 退出码：0 = 全部通过；1 = 有判据不满足（fail-closed）。
 */
'use strict';
const fs = require('fs');
const zlib = require('zlib');
const readline = require('readline');

const file = process.argv[2];
if (!file) { console.error('用法: node check_seed_window.js <capture.csv[.gz]>'); process.exit(2); }

const stream = file.toLowerCase().endsWith('.gz')
    ? fs.createReadStream(file).pipe(zlib.createGunzip())
    : fs.createReadStream(file);

let cols = null;
let prev = { bk: null, sig: null, lat: null, clkg: null, clka: null };
let clkaRiseInLit = 0;           /* 搬运（CLKa 上升沿）落在点亮窗口内的次数 */
let clkaRiseInBlank = 0;         /* 落在消隐窗口内的次数（选项 C 的低亮度落点）*/
let bkHighStart = null;          /* 当前 BK 高（消隐）区间起点 */
let lastT = 0;                   /* 最后一行时间（收尾用） */
const latRise = [];              /* LAT 上升沿时间（量"到帧首锁存的余量"） */
const bkHigh = [];               /* [start, end) 秒 */
const sigSeg = [];               /* {start, end, clkg: [times], latHigh: bool} */
let curSig = null;
const clkgRise = [];             /* 用于量余量的 LAT 前的最后一个 CLKg —— 这里只统计数量 */
let rows = 0;

const rl = readline.createInterface({ input: stream, crlfDelay: Infinity });
rl.on('line', (line) => {
    if (!cols) {
        const h = line.split(',').map((s) => s.trim().toLowerCase());
        cols = {};
        h.forEach((c, i) => { cols[c] = i; });
        if (cols.bk === undefined || cols.sig === undefined || cols.lat === undefined || cols.clkg === undefined) {
            console.error('表头缺少必需列（需要 Time/BK/SIg/LAT/CLKg）：' + h.join('|'));
            process.exit(2);
        }
        return;
    }
    const f = line.split(',');
    const t = parseFloat(f[0]) * 1e6;   /* µs */
    if (!Number.isFinite(t)) return;
    rows++;
    lastT = t;
    const bk = +f[cols.bk], sig = +f[cols.sig], lat = +f[cols.lat], clkg = +f[cols.clkg];
    const clka = cols.clka === undefined ? -1 : +f[cols.clka];
    /* 搬运落点判定（选项 C，见 docs/17）：按 CLKa 上升沿当时 BK 的电平归类 ——
     * 高亮度落点（手册 Figure 5）= 搬运在点亮窗口内；低亮度落点 = 搬运在消隐窗口内。 */
    if (cols.clka !== undefined && prev.clka === 0 && clka === 1) {
        if (bk === 1) clkaRiseInBlank++;
        else clkaRiseInLit++;
    }
    /* BK 区间 */
    if (prev.bk !== null && bk !== prev.bk) {
        if (bk === 1) bkHighStart = t;
        else if (bkHighStart !== null) { bkHigh.push([bkHighStart, t]); bkHighStart = null; }
    }
    if (prev.lat === 0 && lat === 1) latRise.push(t);
    /* SIg 高电平段（= 播种组；帧首每帧一次） */
    if (prev.sig !== null && sig !== prev.sig) {
        if (sig === 1) curSig = { start: t, end: t, clkg: 0, latHigh: false };
        else if (curSig) { curSig.end = t; sigSeg.push(curSig); curSig = null; }
    }
    if (curSig) {
        if (prev.clkg === 0 && clkg === 1) curSig.clkg++;
        if (lat === 1) curSig.latHigh = true;
        curSig.end = t;
    }
    prev = { bk, sig, lat, clkg, clka };
});
rl.on('close', () => {
    if (bkHighStart !== null) { bkHigh.push([bkHighStart, lastT]); bkHighStart = null; } /* 收尾：截到最后一行 */
    /* 播种组：SIg 高电平且其间有 CLKg 脉冲（帧首 5 个脉冲；数据段 SIg 恒 0） */
    const groups = sigSeg.filter((s) => s.clkg > 0 && (s.end - s.start) > 1.0);
    console.log('样本: ' + file + '  (' + rows + ' 行, ' + bkHigh.length + ' 个消隐区间, ' + groups.length + ' 个播种组)');
    if (!groups.length) { console.log('❌ 测不到播种组（SIg 高电平段）⇒ 不通过（fail-closed）'); process.exit(1); }

    let fail = 0;
    groups.forEach((g, i) => {
        /* 判据 A（2026-10-17 口径；选项 C 后两种落点通用）：播种段**不得被 BK 边沿打断**。
         * 新结构把搬运排在点亮窗口内，播种因此落在"搬运结束 → 帧首 wrap"的停等窗口
         * （BK 低）—— 不再是旧结构的"整段落在消隐窗口内"。有 BK 跳变即表示 wrap 打断了
         * 播种（实测症状：SIg 出现两段、栅极链多前进 5 格、画面错位）。 */
        const bkEdgeTimes = bkHigh.flatMap(([a, b]) => [a, b]);
        const interrupted = bkEdgeTimes.some((t) => t > g.start && t < g.end);
        const levelAtStart = bkHigh.some(([a, b]) => a <= g.start && g.start <= b) ? "BK高(消隐)" : "BK低(点亮)";
        const okA = !interrupted;
        const okB = !g.latHigh;
        const okD = g.clkg >= 1;
        const nextLat = latRise.find((x) => x > g.end);
        const margin = nextLat === undefined ? NaN : nextLat - g.end;
        const okC = nextLat !== undefined && margin > 0;
        console.log('  组 ' + (i + 1) + ': ' + g.start.toFixed(2) + '…' + g.end.toFixed(2) + ' µs (宽 '
            + (g.end - g.start).toFixed(2) + ' µs, ' + g.clkg + ' 个 CLKg 上升沿)'
            + '  起点电平 ' + levelAtStart
            + (okA ? '  段内无 BK 边沿 ✅' : '  ❌ 段内被 BK 边沿打断（wrap 打断了播种）')
            + (okB ? '  LAT 全程低 ✅' : '  LAT 期间拉高 ❌')
            + (okD ? '' : '  无 CLKg ❌')
            + (okC ? ('  到下一个 LAT 上升沿余量 ' + margin.toFixed(1) + ' µs ✅') : '  后方无 LAT 上升沿 ❌'));
        if (!okA || !okB || !okD || !okC) fail++;
    });
    const span = bkHigh.length ? (bkHigh[bkHigh.length - 1][1] - bkHigh[0][0]) : 0;
    const blankDuty = span > 0 ? bkHigh.reduce((s, [a, b]) => s + (b - a), 0) / span : 0;
    console.log('  消隐占空比 ≈ ' + (blankDuty * 100).toFixed(1) + '%'
        + '（= 1 − 亮度；手册样例是"两个 ≥5 µs 窄脉冲"，见 docs/17 —— 选项 C 的两种落点见 src/vfd_pio_seed_timing.h）');
    if (clkaRiseInLit + clkaRiseInBlank > 0) {
    const lowPlace = clkaRiseInBlank > clkaRiseInLit;
    console.log('  搬运落点（按 CLKa 上升沿当时 BK 电平）：点亮窗口内 ' + clkaRiseInLit
        + ' / 消隐窗口内 ' + clkaRiseInBlank + ' ⇒ '
        + (lowPlace ? '低亮度落点（搬运整段在消隐内，Note 7② 仍成立）'
                    : '高亮度落点（搬运整段在点亮内 = 手册 Figure 5）'));
}
console.log(fail ? ('❌ ' + fail + ' 个播种组不满足 ⇒ 退出码 1') : '✅ 全部播种组：不被 BK 边沿打断、LAT 全程低、到帧首锁存有正余量 ⇒ 退出码 0');
    process.exit(fail ? 1 : 0);
});
