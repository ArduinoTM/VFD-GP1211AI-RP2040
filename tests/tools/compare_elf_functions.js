#!/usr/bin/env node
/*
 * compare_elf_functions.js —— 比较两个 ELF 的函数是否**逐指令相同**
 *
 * 用途（本项目 2026-10-16 B 类整改的 S1 证据）：证明某次改动"没有碰扫描时序/打包路径" ——
 * 在改动前后的两个提交上各构建同一配置，比较扫描关键函数的反汇编；这些函数必须逐指令一致。
 *
 * 用法：
 *   node tests/tools/compare_elf_functions.js --a <elfA> --b <elfB> [--nm arm-none-eabi-nm]
 *        [--objdump arm-none-eabi-objdump] [--watch a,b,c] [--all]
 *
 * 说明：
 *   · 工具默认从 PATH 找 arm-none-eabi-nm / arm-none-eabi-objdump（也可用 --nm/--objdump 指定全路径）；
 *   · 归一化：丢掉地址列与机器码列；把长度 >= 5 的十六进制串掩码成 ADDR（那是指针/绝对地址，
 *     两个构建之间必然不同）；**保留**小立即数（寄存器偏移、常量）——所以"逐指令相同"仍然是强判据；
 *   · --watch 给"必须逐指令相同"的符号子串（逗号分隔）；任一不满足则退出码 1；
 *   · 末尾打印**全部差异符号清单**（含只在一侧出现的），便于人工确认差异都落在预期位置。
 */
'use strict';
const { execFileSync } = require('child_process');

function argVal(name, def) {
    const i = process.argv.indexOf('--' + name);
    return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : def;
}
const hasFlag = (name) => process.argv.includes('--' + name);

const elfA = argVal('a', null);
const elfB = argVal('b', null);
if (!elfA || !elfB) {
    console.error('用法: node compare_elf_functions.js --a <elfA> --b <elfB> [--watch sym1,sym2] [--all]');
    process.exit(2);
}
const NM = argVal('nm', 'arm-none-eabi-nm');
const OBJDUMP = argVal('objdump', 'arm-none-eabi-objdump');
/* 默认"必须相同"的扫描/打包关键符号（子串匹配，避开 C++ mangling） */
const DEFAULT_WATCH = [
    'scanTick', 'scanTimerThunk', 'vfdStrobe', 'vfdLatPulse', 'vfdSigHoldDelay',
    'seedGrid', 'onFrameDmaDone', 'dmaIrqThunk', 'configureDma',
    'engineInit', 'engineStart', 'engineStop', 'engineDeinit',
    'packScan', 'packFrame', 'wirePrepareFrame', 'packFrameShiftMask', 'packFrameGather',
];
const WATCH = argVal('watch', null) ? argVal('watch', null).split(',').map((s) => s.trim()).filter(Boolean) : DEFAULT_WATCH;

function symbols(file) {
    const out = execFileSync(NM, ['-S', '--defined-only', '--numeric-sort', file], { encoding: 'utf8', maxBuffer: 1 << 28 });
    const list = [];
    for (const line of out.split(/\r?\n/)) {
        const m = /^([0-9a-f]+)\s+([0-9a-f]+)\s+([tTwW])\s+(\S+)$/.exec(line.trim());
        if (m) list.push({ addr: parseInt(m[1], 16), size: parseInt(m[2], 16), name: m[4] });
    }
    return list;
}

/* 把 objdump -d 的输出按 "<addr> <symbol>:" 切块 */
function chunks(file) {
    const text = execFileSync(OBJDUMP, ['-d', '--no-show-raw-insn', file], { encoding: 'utf8', maxBuffer: 1 << 28 });
    const map = new Map();
    let cur = null;
    for (const line of text.split(/\r?\n/)) {
        const head = /^([0-9a-f]+)\s+<([^>]+)>:$/.exec(line);
        if (head) { cur = head[2]; map.set(cur, []); continue; }
        if (cur === null) continue;
        const ins = /^\s*[0-9a-f]+:\s+(.*)$/.exec(line);
        if (!ins) continue;
        /* 归一化：掩码绝对地址（0x 前缀且 >= 5 位十六进制，或裸 >= 5 位十六进制），保留小立即数/偏移；
         * 纯数据占位行（....）统一成 DATA —— 它们不是指令。 */
        let body = ins[1]
            .replace(/0x[0-9a-f]+/gi, (m) => (m.length >= 7 ? 'ADDR' : m))
            .replace(/\b[0-9a-f]{5,}\b/g, 'ADDR')
            .replace(/^[.\s]+$/, 'DATA')
            .replace(/\s+/g, ' ')
            .trim();
        map.get(cur).push(body);
    }
    return map;
}

/* 纯数据块（.word/.byte/... 或只有 DATA）不是指令，单独统计：它们的 .word 是地址，随布局变化很正常 */
const isDataOnly = (lines) => lines.length > 0 && lines.every((l) => /^\.(word|byte|short|long|hword|ascii|asciz)\b/.test(l) || l === 'DATA');

const A = chunks(elfA), B = chunks(elfB);
const symNames = new Set([...A.keys(), ...B.keys()]);
const identical = [], differing = [], onlyA = [], onlyB = [], dataOnly = [];
for (const name of [...symNames].sort()) {
    const a = A.get(name), b = B.get(name);
    if (a && !b) { onlyA.push(name); continue; }
    if (!a && b) { onlyB.push(name); continue; }
    if (isDataOnly(a) && isDataOnly(b)) { dataOnly.push(name); continue; }
    if (a.length === b.length && a.every((v, i) => v === b[i])) identical.push(name);
    else differing.push(name);
}

console.log('A = ' + elfA);
console.log('B = ' + elfB);
console.log('符号总数 ' + symNames.size + '：逐指令相同 ' + identical.length + '，有差异 ' + differing.length
    + '，只在 A ' + onlyA.length + '，只在 B ' + onlyB.length + '，纯数据块（不比较）' + dataOnly.length);
if (differing.length) {
    console.log('\n-- 有差异的函数（最多列 40 个）--');
    for (const n of differing.slice(0, 40)) {
        const a = A.get(n), b = B.get(n);
        let first = -1;
        for (let i = 0; i < Math.max(a.length, b.length); ++i) if (a[i] !== b[i]) { first = i; break; }
        console.log('  ✗ ' + n + '  (指令数 ' + a.length + ' -> ' + b.length + ', 首个差异第 ' + (first + 1) + ' 条)');
        if (hasFlag('all') && first >= 0) {
            console.log('      A: ' + (a[first] || '(无)'));
            console.log('      B: ' + (b[first] || '(无)'));
        }
    }
    if (differing.length > 40) console.log('  … 其余 ' + (differing.length - 40) + ' 个省略（加 --all 看首个差异）');
}
if (onlyA.length) console.log('\n-- 只在 A 里（被删/改名/内联）--\n  ' + onlyA.slice(0, 40).join(', ') + (onlyA.length > 40 ? ' …' : ''));
if (onlyB.length) console.log('\n-- 只在 B 里（新增）--\n  ' + onlyB.slice(0, 40).join(', ') + (onlyB.length > 40 ? ' …' : ''));

console.log('\n-- watch 清单（必须逐指令相同）--');
let fail = 0;
for (const w of WATCH) {
    const hitA = [...A.keys()].filter((n) => n.includes(w));
    const hitB = [...B.keys()].filter((n) => n.includes(w));
    if (!hitA.length && !hitB.length) { console.log('  ? ' + w + ' ：两侧都没有该符号（换个名字？）'); continue; }
    const bad = hitA.filter((n) => !identical.includes(n)).concat(hitB.filter((n) => !identical.includes(n)));
    const uniq = [...new Set(bad)];
    if (uniq.length) { console.log('  ✗ ' + w + ' ：有差异 -> ' + uniq.join(', ')); fail++; }
    else console.log('  ✅ ' + w + ' ：' + hitA.length + ' 个符号逐指令相同');
}
console.log(fail ? ('\n结论：watch 清单里有 ' + fail + ' 项不满足 ⇒ 退出码 1') : '\n结论：watch 清单全部逐指令相同 ⇒ 退出码 0');
process.exit(fail ? 1 : 0);
