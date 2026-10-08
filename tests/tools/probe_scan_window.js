
'use strict';
const fs = require('fs'), readline = require('readline');
(async () => {
  const rl = readline.createInterface({ input: fs.createReadStream(process.argv[2]), crlfDelay: Infinity });
  let hdr = null, prev = null, bks = 0, base = null, printing = false, printed = 0;
  for await (const line of rl) {
    if (!line) continue;
    const p = line.split(',');
    if (!hdr) { hdr = p.map(s => s.trim()); continue; }
    const t = parseFloat(p[0]);
    const v = {}; for (let i = 1; i < hdr.length; ++i) v[hdr[i]] = +p[i];
    if (prev && prev.BK === 0 && v.BK === 1) {   // BK 上升沿 = 消隐起点
      bks++;
      if (bks === 21) { base = t; printing = true; printed = 0;
        console.log('=== 第 21 个扫描：BK 上升沿起 0..3 µs 的全部变化（列顺序同表头）===');
        console.log('   相对µs   CLKa SIa LAT CLKg SIg BK');
      }
    }
    if (printing && base !== null) {
      const rel = (t - base) * 1e6;
      if (rel > 3.0) { printing = false; }
      else { console.log('  ' + rel.toFixed(2).padStart(8) + '    ' + [v.CLKa, v.SIa, v.LAT, v.CLKg, v.SIg, v.BK].join('   ')); printed++;
        if (printed > 30) { printing = false; } }
    }
    prev = v;
  }
})();
