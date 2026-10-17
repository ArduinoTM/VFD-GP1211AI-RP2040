#!/usr/bin/env node
/*
 * check_park_gpio.js —— 从 ELF 反汇编核对「fatal 停泊」与「状态 LED」的 GPIO 写入**确实进了固件**
 *
 * 背景：fatal 停泊（关高压 + 关灯丝驱动）与板载 LED 属于平台层代码，宿主测试用 MockPlatform，
 * 碰不到真正的 RP2040 实现；若不方便上机（人为制造扫描停摆），可以用本脚本在产物级核对：
 * 反汇编里必须能看到对应的 SIO 寄存器写。
 *
 * 用法（两种，**第三方只有 node 也能复跑第二种**）：
 *   node tests/tools/check_park_gpio.js <elf> [--objdump arm-none-eabi-objdump]
 *     （工具默认从 PATH 找 arm-none-eabi-objdump）
 *   node tests/tools/check_park_gpio.js <dump.txt[.gz]>            # 吃入库的反汇编文本
 *   node tests/tools/check_park_gpio.js                            # 不给参数 = 用入库的
 *                                                                  # tests/elf/tick-v1.5.2.txt.gz
 *   反汇编文本用以下命令生成（压缩后入库，见 tests/elf/README.md）：
 *     arm-none-eabi-objdump -d --no-show-raw-insn <elf> | gzip > tests/elf/tick-<版本>.txt.gz
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
 *   F  Rp2040Platform::emergencyOff()（v1.5.2 健壮性修复）：等待必须**不依赖定时器中断** ——
 *      不得调用 sleep_ms()，须直接读硬件计时器（TIMER 基址 0x40054000 / TIMERAWL 0x40054028）忙等；
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
const OBJDUMP = argVal('objdump', 'arm-none-eabi-objdump');

/* 输入可以是 ELF（需要工具链），也可以是入库的 objdump 文本（纯 node，第三方可复跑）*/
function loadDump(target) {
    const fs = require('fs');
    const path = require('path');
    let file = target;
    if (!file) {
        file = path.join(__dirname, '..', 'elf', 'tick-v1.5.2.txt.gz');
        console.log('（未给参数 ⇒ 使用入库反汇编 ' + path.relative(process.cwd(), file) + '，不需要 arm-none-eabi 工具链）');
    }
    if (!fs.existsSync(file)) { console.error('找不到输入文件: ' + file); process.exit(2); }
    const buf = fs.readFileSync(file);
    if (file.endsWith('.gz')) return require('zlib').gunzipSync(buf).toString('utf8');
    if (/\.(txt|lst)$/.test(file)) return buf.toString('utf8');
    return execFileSync(OBJDUMP, ['-d', '--no-show-raw-insn', file], { encoding: 'utf8', maxBuffer: 1 << 28 });
}
const dump = loadDump(elf);
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

/* F（v1.5.2 健壮性修复）：fatal 停泊的等待**不依赖定时器中断** ——
 *    不得调用 sleep_ms()（它依赖默认 alarm pool → TIMER_IRQ_3），必须直接读硬件计时器忙等。 */
{
    const c = pick('Rp2040Platform12emergencyOffEv');
    const hasSleep = c ? c.lines.some((l) => /sleep_ms/.test(l)) : true;
    const hasTimer = c ? c.lines.some((l) => /400540(00|28)/.test(l)) : false;
    judge('F emergencyOff 等待不依赖定时器中断（v1.5.2）', !!c && !hasSleep && hasTimer,
        hasSleep ? '仍调用 sleep_ms：定时器中断失效时停泊会挂在这一句'
            : (hasTimer ? '无 sleep_ms 调用，直接读 TIMER/TIMERAWL 忙等' : '未看到 TIMER 基址，忙等实现可能被改动'),
        c ? c.lines.filter((l) => /400540|sleep/.test(l)).slice(0, 2).join(' | ') : '(找不到 emergencyOff)');
}
console.log(fail ? ('\n结论：' + fail + ' 项不满足 ⇒ 退出码 1') : '\n结论：全部通过 ⇒ 退出码 0');
process.exit(fail ? 1 : 0);
