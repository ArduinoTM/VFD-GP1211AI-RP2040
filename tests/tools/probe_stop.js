
'use strict';
const fs = require('fs'), readline = require('readline');
const file = process.argv[2];
(async () => {
  const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
  let hdr = null, prev = null, n = 0;
  const lastChange = {};        // 每个信号最后一次变化的时间
  const counts = {};            // LAT↑ / CLKa↑ 计数
  const buckets = new Map();    // 10ms 桶里的 LAT↑ 数
  let t0 = null, t1 = null, lastRow = null;
  for await (const line of rl) {
    if (!line) continue;
    const p = line.split(',');
    if (!hdr) { hdr = p.map(s => s.trim()); for (const k of hdr.slice(1)) { lastChange[k] = null; counts[k + '\u2191'] = 0; } continue; }
    const t = parseFloat(p[0]);
    if (t0 === null) t0 = t;
    t1 = t;
    const v = {}; for (let i = 1; i < hdr.length; ++i) v[hdr[i]] = +p[i];
    if (prev) {
      for (const k of hdr.slice(1)) {
        if (prev[k] !== v[k]) lastChange[k] = t;
        if (prev[k] === 0 && v[k] === 1) {
          counts[k + '\u2191'] = (counts[k + '\u2191'] || 0) + 1;
          if (k === 'LAT') { const bk = Math.floor(t / 0.01); buckets.set(bk, (buckets.get(bk) || 0) + 1); }
        }
      }
    }
    prev = v; lastRow = { t, v }; ++n;
  }
  console.log('行数', n, '· 时间范围', t0.toFixed(4), '→', t1.toFixed(4), 's');
  console.log('各信号最后一次变化（相对起点 ms）:');
  for (const k of hdr.slice(1)) console.log('  ', k.padEnd(6), lastChange[k] === null ? '(无)' : ((lastChange[k] - t0) * 1000).toFixed(2) + ' ms');
  console.log('上升沿总数:', JSON.stringify(counts));
  const ks = [...buckets.keys()].sort((a, b) => a - b);
  console.log('LAT↑ 每 10ms 桶:', ks.map(k => k * 10 + 'ms:' + buckets.get(k)).join(' '));
  console.log('最后一行:', JSON.stringify(lastRow));
})();
