
'use strict';
const fs = require('fs'), readline = require('readline');
(async () => {
  const rl = readline.createInterface({ input: fs.createReadStream(process.argv[2]), crlfDelay: Infinity });
  let hdr = null, prev = null;
  let clk = 0;                      // 本扫描内 CLKa↑ 计数
  const perScan = [];               // 每个扫描的 {t, clk, lat}
  let lastLat = null, lastClk = null;
  for await (const line of rl) {
    if (!line) continue;
    const p = line.split(',');
    if (!hdr) { hdr = p.map(s => s.trim()); continue; }
    const t = parseFloat(p[0]);
    const v = {}; for (let i = 1; i < hdr.length; ++i) v[hdr[i]] = +p[i];
    if (prev) {
      if (prev.CLKa === 0 && v.CLKa === 1) clk++;
      if (prev.LAT === 0 && v.LAT === 1) { perScan.push({ t, clk }); clk = 0; }
    }
    prev = v;
  }
  console.log('完整扫描数（LAT↑ 分隔）:', perScan.length);
  const counts = {};
  for (const s of perScan) counts[s.clk] = (counts[s.clk] || 0) + 1;
  console.log('每扫描 CLKa↑ 数分布（期望 384）:', JSON.stringify(counts));
  console.log('前 3 个扫描:', JSON.stringify(perScan.slice(0, 3)));
  console.log('第 100~103 个:', JSON.stringify(perScan.slice(100, 104)));
  console.log('最后 6 个扫描:', JSON.stringify(perScan.slice(-6)));
  const gaps = [];
  for (let i = 1; i < perScan.length; ++i) gaps.push(perScan[i].t - perScan[i - 1].t);
  const us = gaps.map(g => g * 1e6);
  console.log('扫描周期 µs: 最小', Math.min(...us).toFixed(2), '中位', us.slice().sort((a,b)=>a-b)[Math.floor(us.length/2)].toFixed(2), '最大', Math.max(...us).toFixed(2));
  console.log('最后一个扫描 t =', perScan[perScan.length - 1].t.toFixed(4), 's；其后 CLKa 还在跑但无 LAT');
})();
