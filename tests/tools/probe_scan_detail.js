
'use strict';
const fs = require('fs'), readline = require('readline');
(async () => {
  const t0 = parseFloat(process.argv[3]);
  const win = parseFloat(process.argv[4] || '0.0004');
  const rl = readline.createInterface({ input: fs.createReadStream(process.argv[2]), crlfDelay: Infinity });
  let hdr = null, prev = null, printing = false;
  for await (const line of rl) {
    if (!line) continue;
    const p = line.split(',');
    if (!hdr) { hdr = p.map(s => s.trim()); console.log('\u65f6\u95f4(\u00b5s)  ' + hdr.slice(1).map(s => s.padStart(5)).join('')); continue; }
    const t = parseFloat(p[0]);
    if (t < t0 - 0.0002) { prev = p; continue; }
    if (t > t0 + win) break;
    const v = {}; for (let i = 1; i < hdr.length; ++i) v[hdr[i]] = +p[i];
    const chg = [];
    if (prev) for (let i = 1; i < hdr.length; ++i) if (+prev[i] !== +p[i]) chg.push(hdr[i]);
    if (chg.length) console.log((((t - t0) * 1e6).toFixed(2)).padStart(8) + '  ' + hdr.slice(1).map(s => String(v[s]).padStart(5)).join('') + '   ' + chg.join(','));
    prev = p;
  }
})();
