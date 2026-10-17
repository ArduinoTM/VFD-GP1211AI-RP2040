#!/usr/bin/env node
/*
 * check_park_gpio.js —— 从 ELF 反汇编核对「fatal 停泊」与「状态 LED」的 GPIO 写入**确实进了固件**
 *
 * 背景：fatal 停泊（关高压 + 关灯丝驱动）与板载 LED 属于平台层代码，宿主测试用 MockPlatform，
 * 碰不到真正的 RP2040 实现；若不方便上机（人为制造扫描停摆），可以用本脚本在产物级核对：
 * 反汇编里必须能看到对应的 SIO 寄存器写。
 *
 * 用法：
 *   node tests/tools/check_park_gpio.js <elf> [--objdump arm-none-eabi-objdump]
 *   （工具默认从 PATH 找 arm-none-eabi-objdump）
 *
 * RP2040 SIO @ 0xd0000000：GPIO_OUT_SET = +0x14(#20)、GPIO_OUT_CLR = +0x18(#24)、GPIO_OE_SET = +0x24(#36)
 *
 * 判据（全部满足 ⇒ 退出码 0）：
 *   A  defaultRp2040Config()：引脚默认值里含 HV_EN=8 / FL_EN=9（打包字 0x09080706）；
 *   B  Rp2040Platform::emergencyOff()：**先**写 GPIO_OUT_CLR(#24)（HVEN=0）**再**写 GPIO_OUT_SET(#20)
 *      （FLEN=1）—— 顺序即"先关高压、再关灯丝驱动"；
 *   C  Rp2040Platform::setStatusLed()：on/off 两个分支分别写 #20/#24，且 **LED 位 = 1 << VFD_PIN_LED**
 *      确实被算出来（LED 脚号是编译期常量，不进 Rp2040Config —— 见 vfd_platform_rp2040.h 注释）；
 *   D  Rp2040Platform::init()：写 GPIO_OE_SET(#36) ⇒ LED 被配成输出；
 *   E  vfd::Platform::setStatusLed()：空实现（bx lr）⇒ 平台不实现时是 no-op。
 *
 * ⚠️ 本脚本证明的是"路径存在、寄存器与顺序正确"，**不能替代真机电平测量**（脚号是运行时值）。
 * ✅ 2026-10-17 已在实机补做真机电平测量（J-Link 读引脚 + LA 抓包，见 docs/08 §7）。
 */
'use strict';
const { execFileSync } = require('child_process');

function argVal(name, def) {
    const i = process.argv.indexOf('--' + name);
    return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : def;
}
const elf = process.argv.slice(2).find((a) => !a.startsWith('--') && !/objdump/.test(a));
if (!elf) { console.error('用法: node check_park_gpio.js <elf> [--objdump <path>]'); process.exit(2); }
const OBJDUMP = argVal('objdump', 'arm-none-eabi-objdump');

const dump = execFileSync(OBJDUMP, ['-d', '--no-show-raw-insn', elf], { encoding: 'utf8', maxBuffer: 1 << 28 });
const chunks = new Map();
let cur = null;
for (const line of dump.split(/\r?\n/)) {
    const h = /^([0-9a-f]+)\s+<([^>]+)>:$/.exec(line);
    if (h) { cur = h[2]; chunks.set(cur, []); continue; }
    if (!cur) continue;
    const ins = /^\s*[0-9a-f]+:\s+(.*)$/.exec(line);
    if (ins) chunks.get(cur).push(ins[1].replace(/\s+/g, ' ').trim());
}
function pick(sub) { for (const [name, lines] of chunks) if (name.includes(sub)) return { name, lines }; return null; }
function firstIdx(lines, re) { for (let i = 0; i < lines.length; ++i) if (re.test(lines[i])) return i; return -1; }

let fail = 0;
function judge(id, ok, detail, evidence) {
    console.log((ok ? '  ✅ ' : '  ❌ ') + id + '：' + detail);
    if (evidence) console.log('       ' + evidence);
    if (!ok) fail++;
}

console.log('ELF = ' + elf);

/* A：引脚默认值（HV_EN=8 / FL_EN=9 打包在一个 32bit 字里） */
{
    const c = pick('defaultRp2040ConfigEv');
    const hit = c ? c.lines.find((l) => /\.word\s+0x09080706/.test(l)) : null;
    judge('A defaultRp2040Config 引脚默认值', !!hit,
        hit ? 'HV_EN=8 / FL_EN=9 已写入配置（打包字 0x09080706）' : '缺少 HV_EN=8 / FL_EN=9 的打包字',
        hit || (c ? '(找不到 defaultRp2040Config)' : ''));
}
/* 工具：在函数里找"算出 1<<pin 掩码"的序列（movs rX,#a + lsls rX,rX,#s，a<<s == 1<<pin） */
function ledMaskBuilt(lines, pin) {
    const want = (1 << pin) >>> 0;
    const lastMov = {};
    for (const l of lines) {
        const m = /movs\s+(r\d+), #(\d+)/.exec(l);
        if (m) { lastMov[m[1]] = Number(m[2]); continue; }
        const s = /lsls\s+(r\d+), (r\d+), #(\d+)/.exec(l);
        if (s && s[1] === s[2] && lastMov[s[1]] !== undefined) {
            if (((lastMov[s[1]] << Number(s[3])) >>> 0) === want) return l;
        }
    }
    return null;
}
/* B：emergencyOff 的顺序 */
{
    const c = pick('Rp2040Platform12emergencyOffEv');
    const clr = c ? firstIdx(c.lines, /str\s+r\d+, \[r\d+, #24\]/) : -1;
    const set = c ? firstIdx(c.lines, /str\s+r\d+, \[r\d+, #20\]/) : -1;
    const base = c ? firstIdx(c.lines, /lsls\s+r\d+, r\d+, #24/) : -1;
    judge('B emergencyOff 先关高压再关灯丝', !!c && clr >= 0 && set >= 0 && base >= 0 && clr < set,
        clr >= 0 && set >= 0 ? ('GPIO_OUT_CLR(#24) 在第 ' + (clr + 1) + ' 条、GPIO_OUT_SET(#20) 在第 ' + (set + 1) + ' 条'
            + (clr < set ? ' ⇒ 顺序正确（先 HVEN=0、再 FLEN=1）' : ' ⇒ ⚠️ 顺序反了')) : '缺少 SIO 写（#24/#20）',
        c ? [c.lines[clr], c.lines[set]].filter(Boolean).join(' | ') : '(找不到 emergencyOff)');
}
/* C：setStatusLed 的两个分支 + 守卫 */
{
    const c = pick('Rp2040Platform12setStatusLedEb');
    const set = c ? firstIdx(c.lines, /str\s+r\d+, \[r\d+, #20\]/) : -1;
    const clr = c ? firstIdx(c.lines, /str\s+r\d+, \[r\d+, #24\]/) : -1;
    const maskLine = c ? ledMaskBuilt(c.lines, 25) : null;
    judge('C setStatusLed on/off 两分支 + LED 位', !!c && set >= 0 && clr >= 0 && !!maskLine,
        (set >= 0 && clr >= 0) ? ('GPIO_OUT_SET(#20)/CLR(#24) 两条写都在；LED 位（1<<25）由 ' + (maskLine || '？') + ' 算出')
                               : '缺少 GPIO_OUT_SET/CLR 某一条',
        c ? [maskLine, c.lines[set], c.lines[clr]].filter(Boolean).join(' | ') : '(找不到 setStatusLed)');
}
/* D：init 把 LED 配成输出（GPIO_OE_SET = SIO+0x24 = #36） */
{
    const c = pick('Rp2040Platform4initEv');
    const oe = c ? firstIdx(c.lines, /str\s+r\d+, \[r\d+, #36\]/) : -1;
    judge('D init 里 GPIO_OE_SET(#36)', !!c && oe >= 0, oe >= 0 ? '存在输出使能写（GPIO_OE_SET = SIO+0x24）' : '缺少输出使能写',
        c && oe >= 0 ? c.lines[oe] : '(找不到 init)');
}
/* E：Platform::setStatusLed 默认空实现 */
{
    const c = pick('_ZN3vfd8Platform12setStatusLedEb');
    const empty = !!c && c.lines.length <= 2 && c.lines.some((l) => /^bx\s+lr$/.test(l));
    judge('E vfd::Platform::setStatusLed 默认 no-op', empty,
        empty ? '空实现（bx lr）⇒ 平台未实现 LED 时调用无副作用' : '不是空实现（或找不到符号）',
        c ? c.lines.join(' | ') : '(找不到符号)');
}
console.log(fail ? ('\n结论：' + fail + ' 项不满足 ⇒ 退出码 1') : '\n结论：全部通过 ⇒ 退出码 0');
process.exit(fail ? 1 : 0);
