#!/usr/bin/env node
/*
 * 文档/注释数字一致性门禁（2026-10-17 加，第三方评审 N1/N2/D1 的根治办法）。
 *
 * 背景：pio 程序指令数、宿主测试项数这类数字散落在 README / docs / 源码注释 / CI 工作流里，
 * 改代码时没人扫，于是反复漂移（25 vs 26、325 vs 343）。这个脚本把"数字的事实来源"算出来，
 * 再把仓库里所有声称同一事实的数字抓出来比对，不一致即失败（退出码 1）。
 *
 * 事实来源（能在无工具链机器上算的）：
 *   · pio 程序指令数 = src/vfd_scan.pio.h 的 "#define vfd_scan_wrap N" ⇒ N + 1；
 *   · 黄金样本清单 = tests/captures/*.csv.gz 实际文件 vs tests/captures/README.md 的说法；
 *   · 宿主测试项数 = 由 tests/run_host_tests.ps1 在跑完测试后通过 --host-parts/--host-total
 *     传进来（单独运行时跳过这两项，只查前两项）。
 *
 * 用法：
 *   node tests/tools/check_doc_numbers.js
 *   node tests/tools/check_doc_numbers.js --host-parts 79,122,46,53,43 --host-total 343 --handshake-total 79102
 *   node tests/tools/check_doc_numbers.js -v        # 顺带打印每个被检查到的数字
 */
'use strict';
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..', '..');

/* ---- 参数 ------------------------------------------------------------------ */
const args = process.argv.slice(2);
const opt = { verbose: false, hostParts: null, hostTotal: null, handshakeTotal: null };
for (let i = 0; i < args.length; ++i) {
  const a = args[i];
  if (a === '-v' || a === '--verbose') opt.verbose = true;
  else if (a === '--host-parts') opt.hostParts = args[++i].split(',').map(Number);
  else if (a === '--host-total') opt.hostTotal = Number(args[++i]);
  else if (a === '--handshake-total') opt.handshakeTotal = Number(args[++i]);
  else { console.error('未知参数: ' + a); process.exit(2); }
}

const violations = [];
const notices = [];
function fail(file, line, msg) { violations.push(file + ':' + line + ': ' + msg); }

/* ---- 1. pio 程序指令数（唯一事实来源：生成的 .pio.h）------------------------ */
function pioInstructionCount() {
  const h = fs.readFileSync(path.join(root, 'src/vfd_scan.pio.h'), 'utf8');
  const m = h.match(/^#define\s+vfd_scan_wrap\s+(\d+)\s*$/m);
  if (!m) throw new Error('src/vfd_scan.pio.h 里找不到 vfd_scan_wrap');
  return parseInt(m[1], 10) + 1;
}
const pioInsns = pioInstructionCount();
const pioTotal = 32; /* RP2040 每个 PIO 程序最多 32 条指令 */

/* ---- 2. 要扫的文件 ---------------------------------------------------------- */
const SKIP_DIRS = new Set(['.git', 'build', 'node_modules', 'tests/build', '.tools']);
const exts = new Set(['.md', '.html', '.h', '.hpp', '.cpp', '.pio', '.yml', '.yaml']);
const files = [];
(function walk(dir) {
  for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, ent.name);
    const rel = path.relative(root, p).split(path.sep).join('/');
    if (ent.isDirectory()) {
      if (SKIP_DIRS.has(ent.name) || rel === 'tests/build') continue;
      walk(p);
    } else if (exts.has(path.extname(ent.name).toLowerCase())) {
      files.push(rel);
    }
  }
})(root);

/* 历史口径豁免：这些行是在讲"以前是多少"，不要求等于当前值 */
const HIST = /(旧版|此前|曾经|原版|当年|当时|历史|20\d\d-\d\d-\d\d)/;

/* "当前口径"文档白名单：只有这些文件里的测试项数代表"现在"（其余如 docs/07 提交历史、
 * docs/11/16 会话提示词快照、各专项排故报告里的项数是当时的值，属历史记录，不参与比对；
 * pio 指令数与黄金样本清单仍全仓库检查）。 */
const CURRENT_DOCS = /^(README\.md|docs\/(00|08|15)-.*\.(md|html)|\.github\/workflows\/.*\.yml|tests\/captures\/README\.md)$/;

/* ---- 3. 检查：pio 指令数 "N/32" -------------------------------------------- */
for (const rel of files) {
  const lines = fs.readFileSync(path.join(root, rel), 'utf8').split(/\r?\n/);
  lines.forEach((text, idx) => {
    const lineNo = idx + 1;
    /* "扫描/程序/现行 ... N/32"、"程序占 N/32" */
    const re = /(?:扫描|程序|现行)\s*(?:占)?\s*[:：]?\s*\**\s*(\d+)\s*\**\s*\/\s*32/g;
    let m;
    while ((m = re.exec(text)) !== null) {
      const n = parseInt(m[1], 10);
      if (n === pioTotal) continue; /* "32/32 满额" 这类讲满额的不算 */
      const ctx = text.slice(Math.max(0, m.index - 14), m.index);
      if (/从机|slave|SSD1306/i.test(ctx)) continue; /* 从机程序是另一个程序，不在本检查范围 */
      if (HIST.test(text)) { if (opt.verbose) notices.push(rel + ':' + lineNo + ': 历史口径 ' + n + '/32（豁免）'); continue; }
      if (n !== pioInsns)
        fail(rel, lineNo, 'pio 程序指令数写 ' + n + '/32，实际 ' + pioInsns + '/32（取自 src/vfd_scan.pio.h 的 vfd_scan_wrap + 1）');
      else if (opt.verbose) notices.push(rel + ':' + lineNo + ': ' + n + '/32 ✓');
    }
    /* "现行 N 条"（不带 /32 的写法） */
    const re2 = /现行\s*\**\s*(\d+)\s*\**\s*条/g;
    while ((m = re2.exec(text)) !== null) {
      const n = parseInt(m[1], 10);
      if (n !== pioInsns) fail(rel, lineNo, 'pio 程序指令数写"现行 ' + n + ' 条"，实际 ' + pioInsns + ' 条');
    }
  });
}

/* ---- 4. 检查：宿主测试项数 -------------------------------------------------- */
if (opt.hostTotal !== null && opt.hostParts !== null) {
  const total = opt.hostParts.reduce((a, b) => a + b, 0);
  if (total !== opt.hostTotal) {
    fail('(运行时)', 0, '宿主测试自报总数不一致：parts 合计 ' + total + ' ≠ total ' + opt.hostTotal);
  }
  for (const rel of files) {
    if (!CURRENT_DOCS.test(rel) || /.html$/.test(rel)) continue; /* 历史文档、派生的 .html 不查当前项数 */
    const lines = fs.readFileSync(path.join(root, rel), 'utf8').split(/\r?\n/);
    lines.forEach((text, idx) => {
      const lineNo = idx + 1;
      /* (a) 五段加总表达式：79 + 122 + 46 + 53 + 43 = 343 项（或 "共 343 项（79+122+46+53+43）"）*/
      const sumRe = /(\d+)\s*\+\s*(\d+)\s*\+\s*(\d+)\s*\+\s*(\d+)\s*\+\s*(\d+)\s*=\s*(\d+)\s*项/g;
      const sumNoEqRe = /共\s*\**\s*(\d+)\s*\**\s*项\s*[（(]\s*(\d+)\s*\+\s*(\d+)\s*\+\s*(\d+)\s*\+\s*(\d+)\s*\+\s*(\d+)/g;
      let m;
      while ((m = sumRe.exec(text)) !== null) {
        const parts = [1, 2, 3, 4, 5].map((k) => parseInt(m[k], 10));
        const sum = parseInt(m[6], 10);
        if (parts.reduce((a, b) => a + b, 0) !== sum)
          fail(rel, lineNo, '加总表达式算错：' + parts.join('+') + ' = ' + sum + '（应为 ' + parts.reduce((a, b) => a + b, 0) + '）');
        if (HIST.test(text)) continue;
        if (sum !== opt.hostTotal)
          fail(rel, lineNo, '宿主测试总数写 ' + sum + ' 项，实际 ' + opt.hostTotal + ' 项');
      }
      while ((m = sumNoEqRe.exec(text)) !== null) {
        const sum = parseInt(m[1], 10);
        const parts = [2, 3, 4, 5, 6].map((k) => parseInt(m[k], 10));
        if (parts.reduce((a, b) => a + b, 0) !== sum)
          fail(rel, lineNo, '加总表达式算错：共 ' + sum + ' 项 ≠ ' + parts.join('+'));
        if (HIST.test(text)) continue;
        if (sum !== opt.hostTotal)
          fail(rel, lineNo, '宿主测试总数写 ' + sum + ' 项，实际 ' + opt.hostTotal + ' 项');
      }
      /* (b) 三段式：79 + 122 + X + 53 + 43 里的 X 必须等于 pio_seed_timing 项数 */
      const thirdRe = /79\s*\+\s*122\s*\+\s*(\d+)\s*\+\s*53\s*\+\s*43/g;
      while ((m = thirdRe.exec(text)) !== null) {
        if (HIST.test(text)) continue;
        const n = parseInt(m[1], 10);
        if (n !== opt.hostParts[2])
          fail(rel, lineNo, 'pio_seed_timing 项数写 ' + n + '，实际 ' + opt.hostParts[2]);
      }
      /* (c) "宿主…测试/回归… N 项"（N 为三位数以上，避免把子项数当总数）*/
      const totalRe = /(?:宿主机?|全量)[^。\n]{0,20}?(?:测试|回归|用例)[^。\n]{0,12}?(\d{3,})\s*项/g;
      while ((m = totalRe.exec(text)) !== null) {
        if (HIST.test(text)) continue;
        const n = parseInt(m[1], 10);
        if (n !== opt.hostTotal && n !== opt.handshakeTotal)
          fail(rel, lineNo, '宿主测试总数写 ' + n + ' 项，实际 ' + opt.hostTotal + ' 项');
      }
    });
  }
} else {
  notices.push('（未提供 --host-total/--host-parts：跳过"宿主测试项数"检查；CI 里由 run_host_tests.ps1 传入）');
}

/* ---- 5. 检查：核间握手项数 -------------------------------------------------- */
if (opt.handshakeTotal !== null) {
  for (const rel of files) {
    const lines = fs.readFileSync(path.join(root, rel), 'utf8').split(/\r?\n/);
    lines.forEach((text, idx) => {
      const lineNo = idx + 1;
      const re = /(?:核间握手|dualcore_handoff)[^\n]{0,40}?(\d{4,})\s*\**\s*项/g;
      let m;
      while ((m = re.exec(text)) !== null) {
        const n = parseInt(m[1], 10);
        if (HIST.test(text)) continue;
        if (n !== opt.handshakeTotal)
          fail(rel, lineNo, '核间握手项数写 ' + n + '，实际 ' + opt.handshakeTotal);
      }
    });
  }
}

/* ---- 6. 检查：黄金样本清单与实际文件一致（评审 N3）-------------------------- */
const capDir = path.join(root, 'tests/captures');
if (fs.existsSync(capDir)) {
  const actual = fs.readdirSync(capDir).filter((f) => f.endsWith('.csv.gz')).sort();
  const readmePath = path.join(capDir, 'README.md');
  const readme = fs.existsSync(readmePath) ? fs.readFileSync(readmePath, 'utf8') : '';
  const readmeLines = readme.split(/\r?\n/);
  for (const f of actual) {
    const mentioned = readme.includes(f);
    if (!mentioned) fail('tests/captures/README.md', 0, '样本 ' + f + ' 在库里，但 README 没有列出');
  }
  readmeLines.forEach((text, idx) => {
    const re = /golden-[\w.-]+\.csv\.gz/g;
    let m;
    while ((m = re.exec(text)) !== null) {
      const name = m[0];
      const exists = actual.includes(name);
      const off = /(已下线|已删除|不在库|仅存于历史|git 历史)/.test(text);
      if (!exists && !off) fail('tests/captures/README.md', idx + 1, '提到样本 ' + name + ' 但文件不存在（应标注"已下线/已删除"）');
      if (exists && off) fail('tests/captures/README.md', idx + 1, '样本 ' + name + ' 仍在库里，却被写成"已下线/已删除"（改成"保留为已知不合格对照样本"或删文件）');
    }
  });
  if (opt.verbose) notices.push('黄金样本：' + actual.join(', '));
}

/* ---- 输出 ------------------------------------------------------------------ */
console.log('文档数字一致性门禁：pio 程序 ' + pioInsns + '/' + pioTotal + ' 条' +
  (opt.hostTotal !== null ? '，宿主测试 ' + opt.hostTotal + ' 项（' + opt.hostParts.join('+') + '）' : '') +
  (opt.handshakeTotal !== null ? '，核间握手 ' + opt.handshakeTotal + ' 项' : '') + '；扫描 ' + files.length + ' 个文件');
if (opt.verbose) for (const n of notices) console.log('  · ' + n);
if (violations.length === 0) {
  console.log('✅ 全部一致');
  process.exit(0);
}
console.error('❌ 发现 ' + violations.length + ' 处数字与事实不一致：');
for (const v of violations) console.error('  · ' + v);
process.exit(1);
