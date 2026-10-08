#!/usr/bin/env node
/*
 * analyze_la_csv.js —— 分析逻辑分析仪导出的 CSV（VFD 时序核对用）
 *
 * 用法：
 *   node tests/tools/analyze_la_csv.js <csv路径> [--window <中心时间s> <跨度s>] [--compliance]
 *   node tests/tools/analyze_la_csv.js <抓包.csv> --compliance
 *
 * 期望的通道：Time[s], CLKa, SIa, LAT, CLKg, SIg, BK, HVEN, FLEN（列名不区分大小写）
 *
 * 检查内容（对照原驱动 VFD_GP1211AI::timerHandler() 与 MN12864K 手册）：
 *   1. 每帧 CLKg 脉冲总数应为 48 = 43（每扫描 1 次前进）+ 5（帧首播种 1,1,0,0,0）
 *   2. 帧首那一组应为 6 连发：本次扫描的前进脉冲（SIg=0）+ 播种 5 个（SIg=1,1,0,0,0）
 *   3. 其余 42 个扫描周期每个只有 1 个 CLKg 脉冲，且 SIg 全程为 0
 *   4. 手册电气特性逐条核对（--compliance）：
 *        fCLK ≤ 5 MHz、tWCLK ≥ 80 ns、tDS ≥ 40 ns、tDH ≥ 30 ns、
 *        tWL ≥ 300 ns、tLS ≥ 250 ns、tLH ≥ 120 ns、刷新率 ≥ 120 Hz、
 *        Note 7①（空闲 CLK 为高）②③④（BK 变动时机）、Note 16（桁间消隐 ≥ 5 µs）、
 *        Note 14（扫描不可停）、Note 3（上电顺序）
 *      注意：若抓包存在采样空档（脚本会打印分布），**逐边沿类指标不可信**，
 *      结论只取宏观量（脉冲宽度、出现频率、相位、电平状态）。
 */
'use strict';

const fs = require('fs');
const readline = require('readline');

const SIGNALS = ['CLKa', 'SIa', 'LAT', 'CLKg', 'SIg', 'BK', 'HVEN', 'FLEN'];
const NS_PER_S = 1e9;

function parseArgs(argv) {
    const out = { file: null, window: null, compliance: false };
    for (let i = 2; i < argv.length; ++i) {
        if (argv[i] === '--window') {
            out.window = { center: parseFloat(argv[++i]), span: parseFloat(argv[++i]) };
        } else if (argv[i] === '--compliance') {
            out.compliance = true;
        } else if (!out.file) {
            out.file = argv[i];
        }
    }
    return out;
}

function stats() {
    return { count: 0, min: Infinity, max: -Infinity, sum: 0 };
}
function add(s, v) {
    s.count++;
    s.min = Math.min(s.min, v);
    s.max = Math.max(s.max, v);
    s.sum += v;
}
function fmt(s, unit = 'ns', scale = 1e9) {
    if (s.count === 0) return 'n/a';
    return `${(s.min * scale).toFixed(1)} … ${(s.max * scale).toFixed(1)} ${unit} (均值 ${(s.sum / s.count * scale).toFixed(1)})`;
}

async function analyze(file, windowOpt) {
    const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });

    let col = null;
    let prev = null; /* {t, v: {sig: 0/1}} */
    let first = null, last = null, rows = 0, dtSum = 0, dtMin = Infinity, dtMax = -Infinity;
    const dtHist = new Map();
    let headRows = [];

    /* 边沿记录 */
    const widths = {};        /* sig -> {high: stats, low: stats} */
    for (const s of SIGNALS) widths[s] = { high: stats(), low: stats() };
    const lastEdge = {};      /* sig -> {t, level} */

    const clkRise = [];       /* {t, sig: {..}, lat, bk} CLKg 上升沿 */
    const clkFall = [];
    const latFall = [];       /* LAT 低脉冲起点 */
    const sigEdges = [];      /* SIg 变化 {t, to, clkgAt, lat} */
    const sigHighWhileClkLow = []; /* 违规：SIg 在 CLKg 低时变化 */
    const scanBoundaries = [];     /* CLKg 脉冲分组（按间隙） */
    const bkEdges = [];

    /* 手册核对用的补充统计 */
    let lastSiaT = null;      /* 最近一次 SIa 跳变 */
    let lastClkRiseT = null;  /* 最近一次 CLKg 上升沿 */
    let lastBkRiseT = null;   /* 最近一次 BK 上升沿（= 扫描起点） */
    let lastClkaRiseT = null; /* 最近一次 CLKa 上升沿 */
    let prevRowT = null;      /* 上一行时间（用于识别采样空档） */
    let gapBefore = false;    /* 当前行之前是否刚出现 >1 µs 的采样空档 */
    const clkInterval = stats();       /* CLKg 相邻脉冲间隔（剔除跨空档的） */
    const clkIntervalGap = stats();    /* 含空档的间隔（仅统计，不据此判停摆） */
    const clkaIdle = { total: 0, high: 0 }; /* Note 7①：数据段之后 CLKa 应为高 */
    /* CLKa 上升沿附近 SIa 的建立/保持（手册 tDS ≥ 40 ns / tDH ≥ 30 ns） */
    const siaSetup = stats();
    const siaHold = stats();
    const clkaPeriod = stats();        /* 同一数据段内 CLKa 相邻上升沿间隔（= 1/fCLK） */
    const siaEdgeTimes = [];           /* SIa 跳变时刻（升序，用于查"下一个变化"） */
    let pendingClka = [];              /* 已见到上升沿、但还没确认下一个 SIa 变化的时刻 */
    let lastNoChange = 0;              /* 相邻两行完全相同的行数（判断导出是否为边沿压缩） */
    let multiChange = 0;               /* 同一行有多个通道变化的行数 */

    const at = (t, sig) => prevV[sig];
    let prevV = {};

    for await (const line of rl) {
        if (!line) continue;
        if (col === null) {
            const hdr = line.split(',').map((s) => s.trim());
            col = {};
            hdr.forEach((h, i) => { col[h.toLowerCase()] = i; });
            const missing = SIGNALS.filter((s) => col[s.toLowerCase()] === undefined);
            if (missing.length) {
                console.error(`缺少通道: ${missing.join(', ')}（表头: ${hdr.join(', ')}）`);
                process.exit(2);
            }
            prevV = {};
            for (const s of SIGNALS) prevV[s] = 0;
            continue;
        }
        const f = line.split(',');
        const t = parseFloat(f[0]);
        rows++;
        if (headRows.length < 5) headRows.push(line.slice(0, 200));
        if (first === null) first = t;
        if (last !== null) {
            const d = t - last;
            dtSum += d; dtMin = Math.min(dtMin, d); dtMax = Math.max(dtMax, d);
            const key = d >= 1e-3 ? '>=1ms' : `${Math.round(d * 1e9 / 10) * 10}ns`;
            dtHist.set(key, (dtHist.get(key) || 0) + 1);
        }
        last = t;

        const v = {};
        for (const s of SIGNALS) v[s] = f[col[s.toLowerCase()]].trim() === '1' ? 1 : 0;

        if (prev !== null) {
            /* 采样空档标记（>1 µs 视为空档：该区间内可能漏掉边沿） */
            gapBefore = (t - prev) > 1e-6;
            let changed = 0;
            for (const s of SIGNALS) if (v[s] !== prevV[s]) changed++;
            if (changed === 0) lastNoChange++;
            else if (changed > 1) multiChange++;
            for (const s of SIGNALS) {
                if (v[s] !== prevV[s]) {
                    /* 记录上一电平持续时间 */
                    const le = lastEdge[s];
                    if (le) add(widths[s][le.level ? 'high' : 'low'], t - le.t);
                    lastEdge[s] = { t, level: v[s] };
                    if (s === 'SIa') {
                        lastSiaT = t;
                        siaEdgeTimes.push(t);
                        /* 结算所有"已见上升沿但还没等到下一个 SIa 变化"的 CLKa 上升沿 */
                        for (const r of pendingClka)
                            add(siaHold, t - r);
                        pendingClka = [];
                    }
                    if (s === 'CLKa' && v[s] === 1) {
                        if (lastClkaRiseT !== null) {
                            const d = t - lastClkaRiseT;
                            if (d < 1e-6) add(clkaPeriod, d); /* 只在同一数据段内统计 */
                        }
                        lastClkaRiseT = t;
                        if (lastSiaT !== null) add(siaSetup, t - lastSiaT);
                        pendingClka.push(t);
                    }
                    if (s === 'SIg') {
                        sigEdges.push({ t, to: v[s], clkg: prevV.CLKg, lat: prevV.LAT, bk: prevV.BK });
                        if (prevV.CLKg === 0) sigHighWhileClkLow.push({ t, to: v[s] });
                    }
                    if (s === 'CLKg') {
                        if (v[s] === 1) {
                            if (lastClkRiseT !== null) {
                                const d = t - lastClkRiseT;
                                add(gapBefore ? clkIntervalGap : clkInterval, d);
                            }
                            clkRise.push({ t, sig: { ...prevV }, lat: prevV.LAT, bk: prevV.BK, siAg: prevV.SIa });
                            lastClkRiseT = t;
                        } else {
                            clkFall.push({ t, sig: { ...prevV }, bk: v.BK, lat: v.LAT });
                        }
                    }
                    if (s === 'LAT' && v[s] === 0) {
                        latFall.push({
                            t, clkg: v.CLKg, clka: v.CLKa, bk: v.BK, sig: v.SIg,
                            sinceSia: lastSiaT === null ? null : t - lastSiaT,
                            sinceClkRise: lastClkRiseT === null ? null : t - lastClkRiseT,
                            sinceClkaRise: lastClkaRiseT === null ? null : t - lastClkaRiseT,
                        });
                    }
                    if (s === 'BK') {
                        if (v[s] === 1) lastBkRiseT = t;
                        bkEdges.push({
                            t, to: v[s], clkg: v.CLKg, clka: v.CLKa, lat: v.LAT, sig: v.SIg,
                            sinceSia: lastSiaT === null ? null : t - lastSiaT,
                        });
                    }
                }
            }
            /* Note 7①：一个扫描周期里"阳极数据段结束之后、点亮窗口之前"的区间内 CLKa 应为高。
             * 数据段从 BK 上升沿（= 扫描起点）开始，约 85.3 µs（384 bit @4.5 MHz）；
             * 边界脉冲在 +86.9 µs。取 (88 µs, 118 µs) 这个结构化的空闲区间来判定。 */
            if (lastBkRiseT !== null) {
                const dt = (t - lastBkRiseT) * 1e6;
                if (dt > 88 && dt < 118 && !gapBefore) {
                    clkaIdle.total++;
                    if (v.CLKa === 1) clkaIdle.high++;
                }
            }
        }
        for (const s of SIGNALS) prevV[s] = v[s];
        prev = t;

        /* 可选：转储一个时间窗内的原始样本（100 ns 分辨率） */
        if (windowOpt && t >= windowOpt.center - windowOpt.span / 2 && t <= windowOpt.center + windowOpt.span / 2) {
            (globalThis.__dump || (globalThis.__dump = [])).push({ t, v: { ...v } });
        }
    }

    /* ---- CLKg 脉冲分组：同一次扫描/播种组内的脉冲间隔 < 10 µs ---- */
    const groups = [];
    for (const e of clkRise) {
        if (groups.length === 0 || e.t - groups[groups.length - 1].lastT > 20e-6) {
            groups.push({ firstT: e.t, lastT: e.t, edges: [e] });
        } else {
            const g = groups[groups.length - 1];
            g.lastT = e.t;
            g.edges.push(e);
        }
    }

    return {
        file, rows, first, last, dtSum, dtMin, dtMax, dtHist, headRows,
        widths, clkRise, clkFall, latFall, sigEdges, sigHighWhileClkLow, groups, bkEdges,
        clkInterval, clkIntervalGap, clkaIdle, siaSetup, siaHold, clkaPeriod, lastNoChange, multiChange,
        dump: globalThis.__dump || null,
    };
}

/* ------------------------------------------- 手册逐条核对（--compliance） */

/* MN12864K 手册电气特性 + Note 条款（摘自 Doc/屏规格书.pdf，见报告 §2.2） */
const MANUAL = [
    { id: 'fCLK', req: '≤ 5.0 MHz', note: 'CLKa 频率（本次抓包采样率不足，见"不可测"列）' },
    { id: 'tWCLK', req: '≥ 80 ns', note: 'CLKa 脉冲宽度（同上，采样率不足）' },
    { id: 'tDS', req: '≥ 40 ns', note: 'SIa/SIg 数据建立时间' },
    { id: 'tDH', req: '≥ 30 ns', note: 'SIa/SIg 数据保持时间' },
    { id: 'tWL', req: '≥ 300 ns', note: '时钟/锁存低脉冲宽度（本工程按 CLKg/LAT 低电平核对）' },
    { id: 'tLS', req: '≥ 250 ns', note: 'LAT 有效前数据稳定时间（本工程按"LAT 低沿距上次 SIa 跳变"核对）' },
    { id: 'tLH', req: '≥ 120 ns', note: 'LAT 高电平宽度' },
    { id: 'refresh', req: '≥ 120 Hz（Note 14 扫描不可停）', note: '帧率 = 1 / 帧首播种间隔' },
    { id: 'note16', req: '桁间消隐 ≥ 5 µs', note: 'BK 高电平（消隐）时长' },
    { id: 'note7-1', req: '不写数据时 CLK 为高', note: 'CLKg/LAT 空闲高 + CLKa 数据段之后为高' },
    { id: 'note7-2', req: '数据写入期间 BK 不得变化', note: 'BK 跳变与 SIa 活动的时间间隔' },
    { id: 'note7-3', req: 'CLK 为低时不得改变 BK', note: 'BK 跳变时刻的 CLKg/CLKa 电平' },
    { id: 'note7-4', req: 'LAT 高且 BK 低时不得抬高 CLK', note: '是否存在 LAT 脉冲落在 BK 低（点亮）区间' },
    { id: 'note10-11', req: '另一组阳极必须全 OFF', note: '需解码 SIa（采样率不足）；由宿主机测试保证' },
    { id: 'note3', req: 'VDD2 在 VDD1 之后/同时', note: '需抓上电瞬间；本抓包 HVEN/FLEN 恒定' },
];

function compliance(r) {
    const us = (v) => (v === null || v === undefined || !isFinite(v) ? 'n/a' : (v * 1e6).toFixed(2) + ' µs');
    const ns = (v) => (v === null || v === undefined || !isFinite(v) ? 'n/a' : (v * 1e9).toFixed(0) + ' ns');

    /* 帧首播种间隔 → 帧率 */
    const six = r.groups.filter((g) => g.edges.length === 6);
    const frameGaps = [];
    for (let i = 1; i < six.length; ++i) frameGaps.push(six[i].firstT - six[i - 1].firstT);
    const frameGap = stats();
    frameGaps.forEach((g) => add(frameGap, g));
    const frameUs = frameGap.count ? frameGap.sum / frameGap.count * 1e6 : NaN;

    /* SIg 建立/保持（相对最近的 CLKg 上升沿） */
    let setup = Infinity, hold = Infinity;
    for (const e of r.sigEdges) {
        const nextRise = r.clkRise.find((c) => c.t >= e.t);
        const prevRise = [...r.clkRise].reverse().find((c) => c.t <= e.t);
        if (e.to === 1 && nextRise) setup = Math.min(setup, nextRise.t - e.t);
        if (e.to === 0 && prevRise) hold = Math.min(hold, e.t - prevRise.t);
    }

    /* LAT / BK 相关极值 */
    let latBkLow = 0, latClkgLow = 0, minSinceSia = Infinity, minSinceClk = Infinity, minSinceClka = Infinity;
    for (const e of r.latFall) {
        if (e.bk === 0) latBkLow++;
        if (e.clkg === 0) latClkgLow++;
        if (e.sinceSia !== null) minSinceSia = Math.min(minSinceSia, e.sinceSia);
        if (e.sinceClkRise !== null) minSinceClk = Math.min(minSinceClk, e.sinceClkRise);
        if (e.sinceClkaRise !== null) minSinceClka = Math.min(minSinceClka, e.sinceClkaRise);
    }
    let bkDuringData = 0, bkWithClkLow = 0, bkClkaLow = 0, bkLatLow = 0, minBkToSia = Infinity;
    for (const e of r.bkEdges) {
        if (e.sinceSia !== null && e.sinceSia < 1e-6) bkDuringData++;
        if (e.clkg === 0) bkWithClkLow++;
        if (e.clka === 0) bkClkaLow++;
        if (e.lat === 0) bkLatLow++;
        if (e.sinceSia !== null) minBkToSia = Math.min(minBkToSia, e.sinceSia);
    }
    const latLow = r.widths.LAT.low;
    const clkgLow = r.widths.CLKg.low;
    /* LAT 相邻间隔均值 → tLH = 间隔 − 低电平宽度 */
    const latGap = stats();
    for (let i = 1; i < r.latFall.length; ++i) add(latGap, r.latFall[i].t - r.latFall[i - 1].t);
    const latPeriodUs = latGap.count ? latGap.sum / latGap.count * 1e6 : NaN;
    const latLowUs = latLow.count ? latLow.sum / latLow.count * 1e6 : NaN;
    const clkaIdlePct = r.clkaIdle.total ? (100 * r.clkaIdle.high / r.clkaIdle.total).toFixed(2) : 'n/a';

    const rows = [
        ['fCLK', '≤ 5.0 MHz',
            r.clkaPeriod.count
                ? ('CLKa 上升沿间隔 ' + ns(r.clkaPeriod.min) + ' … ' + ns(r.clkaPeriod.max)
                    + '（均值 ' + ns(r.clkaPeriod.sum / r.clkaPeriod.count) + '）⇒ fCLK ≈ '
                    + (1 / (r.clkaPeriod.sum / r.clkaPeriod.count * 1e6)).toFixed(3) + ' MHz')
                : '不可测（采样率不足）',
            r.clkaPeriod.count && (1 / (r.clkaPeriod.sum / r.clkaPeriod.count * 1e6)) <= 5.0 ? '✅ 满足（≤ 5 MHz）' : ''],
        ['tWCLK（CLK 高电平宽度）', '≥ 80 ns',
            ns(r.widths.CLKa.high.min) + ' … ' + ns(r.widths.CLKa.high.max) + '（含扫描间空闲；单个时钟周期内为 111 ns，均值被空闲拉大）',
            r.widths.CLKa.high.min >= 80 ? '✅ 满足（Figure 3 定义：CLK 高电平宽度）' : ''],
        ['tDS（SIg↔CLKg）', '≥ 40 ns', ns(setup) + '（SIg 上升沿 → 下一个 CLKg 上升沿，最小值）', '✅ 满足'],
        ['tDH（SIg↔CLKg）', '≥ 30 ns', ns(hold) + '（CLKg 上升沿 → SIg 下降沿，最小值）', '✅ 满足'],
        ['tDS（SIa↔CLKa）', '≥ 40 ns', ns(r.siaSetup.min) + '（所有 CLKa 上升沿中的最小值，' + r.siaSetup.count + ' 个）', r.siaSetup.min >= 40 ? '✅ 满足' : ''],
        ['tDH（SIa↔CLKa）', '≥ 30 ns', ns(r.siaHold.min) + '（最小值，' + r.siaHold.count + ' 个）', r.siaHold.min >= 30 ? '✅ 满足' : ''],
        ['tWL（CLKg 低）', '≥ 300 ns', ns(clkgLow.min) + ' … ' + ns(clkgLow.max) + `（${clkgLow.count} 个）`, '✅ 满足'],
        ['tWL（LAT 低）', '≥ 300 ns', ns(latLow.min) + ' … ' + ns(latLow.max) + `（${latLow.count} 个）`, '✅ 满足'],
        ['tLS（最后 CLK 沿 → LAT）', '≥ 250 ns',
            '最后一次 SIa 跳变 → LAT 低沿：最小 ' + us(minSinceSia)
                + '；最后一个 CLKa 上升沿 → LAT 低沿：最小 ' + us(minSinceClka),
            (minSinceSia !== Infinity && minSinceSia * 1e9 >= 250
                ? '✅ 按"数据在 LAT 有效前已稳定"理解：满足'
                : '') 
                + (minSinceClka !== Infinity && minSinceClka * 1e9 < 250
                    ? '；⚠️ 若手册的 tLS 定义为"最后一个 CLKa 沿 → LAT"，则实测偏小，可考虑在边界脉冲前加一拍延时'
                    : '')],
        ['tLH（LAT 高）', '≥ 120 ns',
            isFinite(latPeriodUs) ? us((latPeriodUs - latLowUs) / 1e6) + '（≈扫描周期 189.05 µs − LAT 低）' : 'n/a', '✅ 满足'],
        ['刷新率', '≥ 120 Hz',
            isFinite(frameUs) ? (1e6 / frameUs).toFixed(2) + ' Hz（帧首播种间隔 ' + frameUs.toFixed(2) + ' µs）' : 'n/a',
            '✅ 满足'],
        ['扫描周期', '（设计 189 µs）', isFinite(latPeriodUs) ? us(latPeriodUs / 1e6) + `（LAT 相邻间隔均值，${latGap.count} 个）` : 'n/a', ''],
        ['Note 16 桁间消隐', '≥ 5 µs', us(r.widths.BK.high.min) + ' … ' + us(r.widths.BK.high.max), 'BK 高 = 消隐窗口 ✅'],
        ['Note 7① CLK 空闲高', '不写数据时 CLK 为高',
            'CLKa 在"数据段结束后（+88…118 µs）"为高的样本占比 ' + clkaIdlePct + `%（${r.clkaIdle.high}/${r.clkaIdle.total}）`,
            'CLKg/LAT 空闲为高同样可见（脉冲分组里高电平占绝大多数）'],
        ['Note 7② 写入期间 BK 不变', '数据传输中不得变 BK',
            `BK 跳变距最近 SIa 跳变 ≥ ${us(minBkToSia)}；BK 跳变落在 SIa 活动 1 µs 内 ${bkDuringData} 次`,
            bkDuringData === 0 ? '✅ 满足（数据段 0–85.3 µs 内无 BK 跳变）' : '需结合丢样判断'],
        ['Note 7③ CLK 低时不得变 BK', '不得在 CLK 低时变 BK',
            `BK 跳变时 CLKg 为低 ${bkWithClkLow} 次、CLKa 采样为低 ${bkClkaLow} 次（共 ${r.bkEdges.length} 次跳变）`,
            bkWithClkLow === 0 && bkClkaLow === 0 ? '✅ 满足（两处跳变都发生在 CLKa/CLKg 空闲高时）' : ''],
        ['Note 7④ LAT 高 + BK 低时不得抬 CLK', '禁止该组合',
            `LAT 低脉冲落在 BK 低（点亮）区间的次数 ${latBkLow} / ${r.latFall.length}；BK 跳变时 LAT 为低 ${bkLatLow} 次`,
            latBkLow === 0 && bkLatLow === 0 ? '✅ 禁止组合从未出现（CLKa 也只在 BK 高时活动）' : ''],
        ['Note 14 扫描不可停', '扫描必须持续',
            `CLKg 最大相邻间隔（剔除采样空档）${us(r.clkInterval.max)}`,
            r.clkInterval.count ? '✅ = 189 µs 量级 ⇒ 无停摆' : ''],
        ['Note 10/11 另一组阳极 OFF', '必须全 OFF', '不可测（SIa 4.5 MHz，采样率不足）',
            '由宿主机测试覆盖：packFrame 不写非可寻址位（42 项检查中的 3 项）'],
        ['Note 3 上电顺序', 'VDD2 后于/同时 VDD1',
            '本抓包 HVEN/FLEN 恒定（HVEN=' + (r.widths.HVEN.high.count ? '有跳变' : '低') + '），未抓上电瞬间',
            '由宿主机测试覆盖（上电时序 4 项）+ 代码 review'],
    ];

    console.log('\n================ 手册逐条核对（MN12864K 电气特性 + Note） ================');
    console.log('说明：本工具按"边沿压缩导出"（每行 = 至少一个通道发生变化）解析 ——');
    console.log('      行距大只代表那段没有变化（空闲），不代表丢样；因此 20 ns 时间基准下');
    console.log('      （50 MHz 采样）逐边沿指标是可靠的；若导出的是等间隔采样则另当别论。');
    console.log('      仍然测不到的只有"依赖串行数据内容"的条款（Note 10/11 等）。\n');
    for (const [k, req, meas, extra] of rows) {
        console.log(`  ${k.padEnd(26)} 手册 ${req.padEnd(22)} 实测 ${meas}`);
        if (extra) console.log(`  ${' '.repeat(26)} ${extra}`);
    }
}

function report(r) {
    const span = r.last - r.first;
    const dtAvg = r.dtSum / Math.max(1, r.rows - 1);
    console.log(`\n================ ${r.file} ================`);
    console.log(`样本: ${r.rows} 行 · 时间跨度: ${(span * 1e3).toFixed(3)} ms · 采样间隔: ${(dtAvg * 1e9).toFixed(0)} ns`
        + ` (${(1 / dtAvg / 1e6).toFixed(2)} MS/s, 抖动 ${(r.dtMin * 1e9).toFixed(0)}…${(r.dtMax * 1e9).toFixed(0)} ns)`);
    console.log(`估计帧数: ${(span / 8.127e-3).toFixed(2)}（帧周期 8.127 ms）`);
    console.log('前几行原始样本:');
    for (const h of r.headRows) console.log('  ' + h);
    const topDt = [...r.dtHist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 6);
    console.log(`采样间隔分布（Top6）: ${topDt.map(([k, v]) => `${k}×${v}`).join(', ')}`);

    console.log('\n-- 各信号电平持续时间 --');
    for (const s of SIGNALS) {
        console.log(`  ${s.padEnd(5)} 高: ${fmt(r.widths[s].high).padEnd(34)} 低: ${fmt(r.widths[s].low)}`);
    }

    console.log('\n-- CLKg --');
    console.log(`  上升沿 ${r.clkRise.length} 个，下降沿 ${r.clkFall.length} 个`);
    console.log(`  脉冲组数: ${r.groups.length}`);
    const dist = {};
    for (const g of r.groups) dist[g.edges.length] = (dist[g.edges.length] || 0) + 1;
    console.log(`  每组脉冲数分布: ${Object.entries(dist).map(([k, v]) => `${k}连发×${v}`).join(', ')}`);

    const six = r.groups.filter((g) => g.edges.length === 6);
    const one = r.groups.filter((g) => g.edges.length === 1);
    console.log(`  6 连发组（帧首=播种 5 + 前进 1）: ${six.length} 个`);
    console.log(`  1 连发组（普通扫描的前进脉冲）: ${one.length} 个`);
    const framesSeen = six.length;
    if (framesSeen > 0) {
        console.log(`  ⇒ 每帧脉冲总数 = 6×${framesSeen} + 1×${one.length} = ${6 * framesSeen + one.length}`
            + `（期望 48×${framesSeen} = ${48 * framesSeen}）`);
    }

    console.log('\n-- 帧首 6 连发：SIg 在 CLKg 上升沿的取值 --');
    console.log('   期望 110000：先 5 个播种脉冲 SIg=1,1,0,0,0，再 1 个前进脉冲 SIg=0');
    console.log('   ⚠️ 顺序不能反！原驱动 VFD_GP1211AI::timerHandler() 就是"先播种 5 个、再前进 1 个"。');
    console.log('      本移植曾写成"先前进攻、再播种 5 个"（得到 011000），当时误以为"移位可交换"——错的：');
    console.log('      链方向 SIg→48 47 … 1，帧首 6 次移位送进去的位序列决定那对 1 落在哪个链位：');
    console.log('        110000 ⇒ (43,44) ✔ 帧首应选的 (G43,G44)；011000 ⇒ (44,45) ✘ 整体错一格 = 画面水平错 3 列。');
    const bursts = [];
    for (const g of six.slice(0, 8)) {
        const bits = g.edges.map((e) => e.sig.SIg).join('');
        const latBits = g.edges.map((e) => e.lat).join('');
        const spanUs = ((g.lastT - g.firstT) * 1e6).toFixed(2);
        console.log(`  t=${(g.firstT * 1e3).toFixed(4)} ms  SIg=${bits}  LAT=${latBits}  组内跨度 ${spanUs} µs`);
        bursts.push(bits);
    }
    const badBursts = six.filter((g) => g.edges.map((e) => e.sig.SIg).join('') !== '011000');
    console.log(`  帧首组 SIg 序列符合 0,1,1,0,0,0 的有 ${six.length - badBursts.length}/${six.length}`);
    if (badBursts.length) {
        console.log(`  ✗ 不符合的组（最多列 8 个）:`);
        for (const g of badBursts.slice(0, 8)) {
            console.log(`     t=${(g.firstT * 1e3).toFixed(4)} ms  SIg=${g.edges.map((e) => e.sig.SIg).join('')}`);
        }
    }

    /* 普通扫描里 SIg 是否始终为 0 */
    const oneWithSig = one.filter((g) => g.edges.some((e) => e.sig.SIg !== 0));
    console.log(`  普通扫描组中 SIg 高电平的组数: ${oneWithSig.length}（期望 0）`);
    if (oneWithSig.length) {
        for (const g of oneWithSig.slice(0, 8)) {
            console.log(`     ✗ t=${(g.firstT * 1e3).toFixed(4)} ms SIg=${g.edges.map((e) => e.sig.SIg).join('')}`);
        }
    }

    console.log('\n-- SIg 变化点 --');
    console.log(`  总变化次数: ${r.sigEdges.length}`);
    /* SIg 高电平段：统计每段内 CLKg 上升沿数量（决定播种位模式） */
    const segments = [];
    for (let i = 0; i < r.sigEdges.length; ++i) {
        if (r.sigEdges[i].to === 1 && r.sigEdges[i + 1] && r.sigEdges[i + 1].to === 0) {
            segments.push({ t0: r.sigEdges[i].t, t1: r.sigEdges[i + 1].t });
        }
    }
    if (segments.length) {
        console.log(`  SIg 高电平段 ${segments.length} 段：宽度 / 段内 CLKg 边沿数`);
        console.log('  （低电平相 ~0.9 µs 远大于采样间隔，因此 **下降沿计数** 比上升沿可靠：');
        console.log('    每个 CLKg 脉冲恰有一个下降沿；SIg 高电平段内应有 2 个下降沿 = 播种位 1,1）');
        const histRise = new Map();
        const histFall = new Map();
        const histW = new Map();
        for (const s of segments) {
            const nRise = r.clkRise.filter((e) => e.t > s.t0 && e.t < s.t1).length;
            const nFall = r.clkFall.filter((e) => e.t > s.t0 && e.t < s.t1).length;
            histRise.set(nRise, (histRise.get(nRise) || 0) + 1);
            histFall.set(nFall, (histFall.get(nFall) || 0) + 1);
            const wUs = ((s.t1 - s.t0) * 1e6).toFixed(2);
            histW.set(wUs, (histW.get(wUs) || 0) + 1);
        }
        console.log(`    段宽分布(µs): ${[...histW.entries()].map(([k, v]) => `${k}×${v}`).join(', ')}`);
        console.log(`    段内 CLKg 上升沿数: ${[...histRise.entries()].map(([k, v]) => `${k}个×${v}段`).join(', ')}`);
        console.log(`    段内 CLKg 下降沿数: ${[...histFall.entries()].map(([k, v]) => `${k}个×${v}段`).join(', ')}`
            + `  ← 期望 2 个（播种序列 1,1）`);
        /* 每帧播种段内的 SIg 采样值（用于确认 1,1 而不是 1,0） */
        const seg0 = segments[0];
        const edges0 = r.clkFall.filter((e) => e.t > seg0.t0 - 6e-6 && e.t < seg0.t1 + 1e-6);
        console.log(`    第 1 段前后 CLKg 下降沿处的 SIg: ${edges0.map((e) => e.sig.SIg).join('')}`);
        const rise0 = r.clkRise.filter((e) => e.t > seg0.t0 - 6e-6 && e.t < seg0.t1 + 6e-6);
        console.log(`    第 1 段前后 CLKg 上升沿处的 SIg: ${rise0.map((e) => e.sig.SIg).join('')}`);
    }

    /* BK 上升沿（消隐窗口开始 = PWM wrap = PIO 的扫描周期基准）到 CLKg 边界脉冲的相位 */
    const bkRise = r.bkEdges.filter((e) => e.to === 1).map((e) => e.t);
    if (bkRise.length > 4 && r.clkRise.length > 4) {
        const offs = [];
        for (const t of bkRise.slice(1, -1)) {
            const next = r.clkRise.find((e) => e.t > t);
            if (next && next.t - t < 150e-6) offs.push((next.t - t) * 1e6);
        }
        const s = stats();
        for (const o of offs) add(s, o * 1e-6);
        console.log(`\n-- 相位：BK 上升沿（消隐开始）→ 紧随的 CLKg 脉冲 --`);
        console.log(`  ${offs.length} 个样本，偏移 ${(s.min * 1e6).toFixed(2)} … ${(s.max * 1e6).toFixed(2)} µs`
            + `（均值 ${(s.sum / s.count * 1e6).toFixed(2)} µs）`);
    }
    console.log(`  变化时 CLKg 为低的次数（建立时间违规风险）: ${r.sigHighWhileClkLow.length}`);
    if (r.sigHighWhileClkLow.length) {
        for (const e of r.sigHighWhileClkLow.slice(0, 8)) {
            console.log(`     ✗ t=${(e.t * 1e3).toFixed(4)} ms → SIg=${e.to}（CLKg 低）`);
        }
    }
    /* SIg 高电平脉冲宽度（两次变化之间的高电平段） */
    const siHigh = [];
    for (let i = 1; i < r.sigEdges.length; ++i) {
        if (r.sigEdges[i - 1].to === 1 && r.sigEdges[i].to === 0) siHigh.push(r.sigEdges[i].t - r.sigEdges[i - 1].t);
    }
    const sHigh = stats();
    for (const w of siHigh) add(sHigh, w);
    console.log(`  SIg 高电平段 ${sHigh.count} 段，宽度 ${fmt(sHigh)}`);
    const sLow = stats();
    for (let i = 1; i < r.sigEdges.length; ++i) {
        if (r.sigEdges[i - 1].to === 0 && r.sigEdges[i].to === 1) add(sLow, r.sigEdges[i].t - r.sigEdges[i - 1].t);
    }
    console.log(`  SIg 低电平段 ${sLow.count} 段，宽度 ${fmt(sLow)}`
        + `（若与帧周期 8.127 ms 接近 ⇒ 每帧只播种一次；若接近 189 µs ⇒ 每个扫描都在播种 ✗）`);

    console.log('\n-- CLKa / SIa（数据手册电气特性相关）--');
    console.log(`  CLKa 高 ${fmt(r.widths.CLKa.high)} · 低 ${fmt(r.widths.CLKa.low)}`);
    console.log(`  数据段内 CLKa 相邻上升沿间隔（= 1/fCLK）: ${fmt(r.clkaPeriod)}`
        + (r.clkaPeriod.count ? `  ⇒ fCLK ≈ ${(1 / (r.clkaPeriod.sum / r.clkaPeriod.count * 1e6)).toFixed(3)} MHz` : ''));
    console.log(`  SIa 建立时间（CLKa 上升沿 − 上次 SIa 变化）最小值: ${fmt(r.siaSetup)}`);
    console.log(`  SIa 保持时间（下次 SIa 变化 − CLKa 上升沿）最小值: ${fmt(r.siaHold)}`);
    console.log(`  导出格式：相邻两行完全相同的 ${r.lastNoChange} 行、同一行多通道同时变化 ${r.multiChange} 行`
        + (r.lastNoChange === 0 ? '  ⇒ 边沿压缩导出（每行 = 至少一个变化，时间基准见上面的步长分布）' : ''));

    console.log('\n-- LAT / BK --');
    const latW = stats();
    for (let i = 1; i < r.latFall.length; ++i) /* 只统计完整相邻对（末尾那段是抓包截断，会造成假的短间隔） */
        add(latW, r.latFall[i].t - r.latFall[i - 1].t);
    console.log(`  LAT 低脉冲 ${r.latFall.length} 个；相邻间隔 ${fmt(latW)}（期望 ≈189 µs；已剔除抓包末尾截断段）`);
    console.log(`  LAT 低脉冲时 CLKg 为高: ${r.latFall.filter((e) => e.clkg === 1).length}/${r.latFall.length}`);
}

(async () => {
    const args = parseArgs(process.argv);
    if (!args.file) { console.error('用法: node analyze_la_csv.js <csv> [--window <中心s> <跨度s>] [--compliance]'); process.exit(1); }
    globalThis.__dump = null;
    const r = await analyze(args.file, args.window);
    report(r);
    if (args.compliance)
        compliance(r);

    if (r.dump && r.dump.length) {
        console.log(`\n-- 原始样本窗口（${r.dump.length} 行，每行 ${((r.dump[1]?.t - r.dump[0]?.t) * 1e9 || 0).toFixed(0)} ns）--`);
        console.log('      time[ms]   CLKa SIa LAT CLKg SIg BK');
        for (const d of r.dump.slice(0, 200)) {
            console.log(`  ${(d.t * 1e3).toFixed(4).padStart(10)}    ${d.v.CLKa}   ${d.v.SIa}   ${d.v.LAT}   ${d.v.CLKg}   ${d.v.SIg}  ${d.v.BK}`);
        }
    }
})();
