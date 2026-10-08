
'use strict';
const fs = require('fs'), readline = require('readline');
const file = process.argv[2];
(async () => {
  const rl = readline.createInterface({ input: fs.createReadStream(file), crlfDelay: Infinity });
  let hdr = null, prev = null;
  const ev = [];                       // 事件：类型 + 时间
  for await (const line of rl) {
    if (!line) continue;
    const p = line.split(',');
    if (!hdr) { hdr = p.map(s => s.trim()); continue; }
    const t = parseFloat(p[0]);
    const v = { CLKa: +p[1], SIa: +p[2], LAT: +p[3], CLKg: +p[4], SIg: +p[5], BK: +p[6] };
    if (prev) {
      const cmp = [['LAT', 'LAT'], ['CLKg', 'CLKg'], ['CLKa', 'CLKa'], ['BK', 'BK'], ['SIg', 'SIg']];
      for (const [k] of cmp) {
        if (prev[k] === 0 && v[k] === 1) ev.push({ t, e: k + '↑' });
        if (prev[k] === 1 && v[k] === 0) ev.push({ t, e: k + '↓' });
      }
    }
    prev = v;
  }
  ev.sort((a, b) => a.t - b.t);
  // 找所有 LAT 上升沿（新极性下的"锁存脉冲起点"）
  const latUp = ev.filter(e => e.e === 'LAT↑');
  console.log('文件', file.split('\\').pop());
  console.log('LAT 上升沿（锁存脉冲）个数:', latUp.length, '· 事件总数', ev.length);
  const rel = { lat2clkg: [], latHold: [], clkgBeforeLat: 0, prevDataEnd2Lat: [] };
  for (let i = 0; i < latUp.length; ++i) {
    const t0 = latUp[i].t, t1 = (i + 1 < latUp.length) ? latUp[i + 1].t : Infinity;
    const win = ev.filter(e => e.t >= t0 - 2000e-9 && e.t < t1);
    const latDown = win.find(e => e.e === 'LAT↓');
    if (latDown) rel.latHold.push((latDown.t - t0) * 1e9);
    // 本窗口内的 CLKg 上升沿（栅极前进）
    const clkgUp = win.filter(e => e.e === 'CLKg↑' && e.t >= t0).map(e => e.t);
    if (clkgUp.length) {
      rel.lat2clkg.push((clkgUp[0] - t0) * 1e9);
      if (clkgUp[0] < t0) rel.clkgBeforeLat++;
    }
    // 上个窗口最后一个 CLKa 上升沿 → 本次 LAT↑（tLS）
    const prevWinClk = ev.filter(e => e.e === 'CLKa↑' && e.t < t0 && e.t > t0 - 189e-6);
    if (prevWinClk.length) rel.prevDataEnd2Lat.push((t0 - prevWinClk[prevWinClk.length - 1].t) * 1e9);
  }
  const stat = (a) => a.length ? `n=${a.length} 最小 ${Math.min(...a).toFixed(0)} ns 中位 ${a.slice().sort((x,y)=>x-y)[Math.floor(a.length/2)].toFixed(0)} ns 最大 ${Math.max(...a).toFixed(0)} ns` : '(无)';
  console.log('LAT 脉冲宽度 (LAT↑→↓):', stat(rel.latHold), '（手册 tWL ≥ 300 ns）');
  console.log('LAT↑ → 紧随的 CLKg↑:', stat(rel.lat2clkg), '（>0 = 先锁存后前进；<0 = 先前进攻）');
  console.log('前一次 CLKa↑ → 本次 LAT↑（tLS 口径）:', stat(rel.prevDataEnd2Lat), '（手册 tLS ≥ 250 ns）');
  // 每扫描的 CLKa 个数 + 数据段起点/终点相对 LAT↑
  const bkUp = ev.filter(e => e.e === 'BK↑');
  console.log('BK↑（消隐开始）个数:', bkUp.length);
  if (bkUp.length > 2) {
    const t0 = bkUp[1].t, t1 = bkUp[2].t;
    const seg = ev.filter(e => e.t >= t0 && e.t < t1);
    const firstClk = seg.find(e => e.e === 'CLKa↑');
    const lastClk = seg.filter(e => e.e === 'CLKa↑').pop();
    const lat = seg.find(e => e.e === 'LAT↑');
    const clkg = seg.find(e => e.e === 'CLKg↑');
    console.log('一个扫描内（相对 BK↑ 消隐起点，µs）:');
    const rel1 = (t) => ((t - t0) * 1e6).toFixed(3);
    if (lat) console.log('   LAT↑      ', rel1(lat.t));
    if (clkg) console.log('   CLKg↑(前进)', rel1(clkg.t));
    if (firstClk) console.log('   首个 CLKa↑ ', rel1(firstClk.t));
    if (lastClk) console.log('   末个 CLKa↑ ', rel1(lastClk.t));
    console.log('   本扫描 CLKa 上升沿数:', seg.filter(e => e.e === 'CLKa↑').length, '（期望 384）');
  }
})();
