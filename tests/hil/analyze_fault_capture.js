#!/usr/bin/env node
/*
 * analyze_fault_capture.js —— 分析 fatal 停泊的 LA 抓包（本仓库 tests/hil/ 的 HIL 流程第 3 步）
 *
 * 用法：
 *   node tests/hil/analyze_fault_capture.js <抓包.csv>
 *
 * 输入：逻辑分析仪导出的边沿压缩 CSV，表头形如
 *   Time[s], CLKa, SIa, LAT, CLKg, SIg, BK, HVEN, FLEN
 *   （触发建议设在 HVEN 下降沿；务必等板子启动完成后再布防，否则会抓到上电时序）
 *
 * 输出：各关键信号的跳变时刻（µs）、BK 最后一次跳变、CLKa 每 20 ms 的边沿数（用于判断扫描是否停住）。
 * 判据（对照 tests/hil/README.md）：
 *   · HVEN 只有 1 次 1→0；FLEN 只有 1 次 0→1 且比 HVEN 晚 ~µs 级；
 *   · BK 在 HVEN↓ 后不久停止跳变（恒高 = 全消隐）；
 *   · CLKa 边沿数在停泊后 → 0（扫描停）；
 *   · 全部时间以 HVEN↓ 所在行为 t=0（触发点）。
 */
'use strict';
const fs = require('fs'), readline = require('readline');
const file = process.argv[2];
if (!file) { console.error('用法: node tests/hil/analyze_fault_capture.js <抓包.csv>'); process.exit(2); }
const SIG = ['CLKa','SIa','LAT','CLKg','SIg','BK','HVEN','FLEN'];
const rows = [];
/* 输入可以是 .csv，也可以是 .csv.gz（抓包体积大，归档时压缩；纯 node 解压）*/
let input;
if (/\.gz$/.test(file)) {
    const { PassThrough } = require('stream');
    input = new PassThrough();
    input.end(require('zlib').gunzipSync(fs.readFileSync(file)));
} else {
    input = fs.createReadStream(file);
}
const rl = readline.createInterface({ input, crlfDelay: Infinity });
let hdr = null;
rl.on('line', (line) => {
  if (!line) return;
  if (!hdr) { hdr = line.split(',').map((s) => s.trim()); return; }
  const f = line.split(',');
  rows.push([parseFloat(f[0]), f.slice(1).map((s) => parseInt(s, 10))]);
});
rl.on('close', () => {
  if (rows.length < 2) { console.error('抓包为空'); process.exit(1); }
  console.log('文件=' + file.split(/[\\/]/).pop() + ' 行数=' + rows.length +
    ' 时间 ' + rows[0][0].toFixed(6) + ' … ' + rows[rows.length - 1][0].toFixed(6) + ' s');
  const trans = {}; for (const s of SIG) trans[s] = [];
  const prev = rows[0][1].slice();
  for (let i = 1; i < rows.length; ++i) {
    for (let k = 0; k < SIG.length; ++k) {
      const v = rows[i][1][k];
      if (v !== prev[k]) { trans[SIG[k]].push([rows[i][0], prev[k], v]); prev[k] = v; }
    }
  }
  for (const s of ['HVEN', 'FLEN', 'BK']) {
    console.log('--- ' + s + ' 跳变（' + trans[s].length + ' 次）---');
    for (const [t, a, b] of trans[s].slice(0, 6)) console.log('   t=' + (t * 1e6).toFixed(1) + ' µs  ' + a + '→' + b);
    if (trans[s].length > 6) console.log('   …（共 ' + trans[s].length + ' 次，最后一次 t=' + (trans[s][trans[s].length - 1][0] * 1e6).toFixed(1) + ' µs）');
  }
  const bins = new Map();
  for (const [t] of trans['CLKa']) { const b = Math.floor(t * 50) / 50; bins.set(b, (bins.get(b) || 0) + 1); }
  console.log('--- CLKa 20 ms 分桶边沿数（停泊后应降到 0）---');
  for (const k of [...bins.keys()].sort((a, b) => a - b)) console.log('   t=' + (k * 1000).toFixed(0) + ' ms : ' + bins.get(k));
});