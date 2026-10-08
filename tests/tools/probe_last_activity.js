
'use strict';
const fs = require('fs'), readline = require('readline');
(async () => {
  const rl = readline.createInterface({ input: fs.createReadStream(process.argv[2]), crlfDelay: Infinity });
  let hdr = null, prev = null;
  const last = {}, counts = {}, buckets = {};
  let tEnd = 0, n = 0, tStart = 0;
  const B = 0.02; // 20 ms 一桶
  for await (const line of rl) {
    if (!line) continue;
    const p = line.split(',');
    if (!hdr) { hdr = p.map(s => s.trim()); continue; }
    const t = parseFloat(p[0]);
    const v = {}; for (let i = 1; i < hdr.length; ++i) v[hdr[i]] = +p[i];
    if (n === 0) tStart = t;
    if (prev) for (const k of Object.keys(v)) if (prev[k] !== v[k]) {
      counts[k] = (counts[k] || 0) + 1; last[k] = t;
      const bi = Math.floor((t - tStart) / B);
      const key = k + '@' + bi;
      buckets[key] = (buckets[key] || 0) + 1;
    }
    prev = v; tEnd = t; n++;
  }
  console.log('行数', n, '· 时长', ((tEnd - tStart) * 1e3).toFixed(1), 'ms  (时间列单位 = 秒)');
  for (const k of Object.keys(last))
    console.log('  ' + k.padEnd(5), '翻转', String(counts[k]).padStart(7), '· 末次', ((last[k] - tStart) * 1e3).toFixed(1), 'ms');
  console.log('\n每 20 ms 的翻转数（LAT/CLKg/SIg/CLKa/BK）：');
  const nb = Math.ceil((tEnd - tStart) / B);
  for (let i = 0; i < nb; ++i) {
    const row = ['LAT', 'CLKg', 'SIg', 'CLKa', 'BK'].map(k => String(buckets[k + '@' + i] || 0).padStart(6)).join(' ');
    console.log('  ' + String(i * B * 1e3).padStart(6) + ' ms |' + row);
  }
  console.log('\n最后 6 行原始数据：');
  const all = fs.readFileSync(process.argv[2], 'utf8').trim().split(/\r?\n/);
  console.log(all.slice(-6).join('\n'));
})();
