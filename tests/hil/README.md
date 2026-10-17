# tests/hil —— 硬件在环（HIL）测试脚本：fatal 停泊（B1）

本目录归档 2026-10-17 上机验证 **fatal 停泊（扫描看护连续失败 → 紧急关断）** 用的脚本与流程。
验证目标 = 第三方评审 D2 那条"安全兜底未上机确认"：**关高压（HVEN=0）+ 关断灯丝驱动（FLEN=1）
+ BK 全消隐 + 停扫描 + `_fatal=1` + 板载 LED 常亮**，且在真实故障注入下由**固件自己的看护路径**走到。

> ⚠️ **必须在"未接面板"的测试板上做**：本流程会人为制造扫描停摆/定时器中断失效，接面板时有毁屏风险。

## 需要什么

* SEGGER J-Link + `JLink.exe`（本次用 V8.48，SWD 4 MHz）与接好 SWD 的 RP2040 板；
* 一份固件 ELF（`build-rp2040*/vfd_gp1211ai_demo.elf`）；
* 可选：逻辑分析仪（HVEN/FLEN/BK/CLKa/CLKg/LAT/SIg 8 路，触发设 **HVEN 下降沿**）。

## 三种触发方式（都只写寄存器，不调用任何固件函数）

| 方式 | 注入 | 走到的结果 | 备注 |
|---|---|---|---|
| **pio 引擎 · 关 DMA 中断** | `NVIC_ICER0 = bit11`（DMA_IRQ_0）| 扫描帧进度冻结 → 看护 4 次判停 → recover 无效 → fatal 停泊 | `fault_pio.jlink`；一次成功，LED 常亮 |
| **tick 引擎 · 关定时器中断** | `NVIC_ICER0 = bit3`（TIMER_IRQ_3）| 扫描停摆 → 看护 4 次判停 → recover 无效 → fatal 停泊 | `fault_tick.jlink`；安全三动作立即生效，`engineStop()/_fatal/LED` 需一次 WFE 唤醒（脚本里用 halt/resume 制造） |
| tick 引擎 · 直调 `emergencyOff()`（早期验证，已不推荐）| 用调试器改 PC 调用真实函数 | 停泊动作 + 电平实测 | 验证过但**不能证明看护链路**，脚本已删除 |

## 步骤

```
# 1) 烧固件（ELF 路径在 flash.jlink 里改；在仓库根目录运行）
"<JLink.exe>" -Device RP2040_M0_0 -If SWD -Speed 4000 -autoconnect 1 -ExitOnError 1 -CommanderScript tests/hil/flash.jlink
# 2) 等板子启动完成（≥2.5 s；此时 HV_EN=1 / FL_EN=0）
# 3) 布防 LA（触发 = HVEN 下降沿）。⚠️ 务必在启动完成后再布防，否则会抓到上电时序（见下"坑"）
# 4) 注入故障 + 读回（pio / tick 二选一）
"<JLink.exe>" -Device RP2040_M0_0 -If SWD -Speed 4000 -autoconnect 1 -CommanderScript tests/hil/fault_pio.jlink
"<JLink.exe>" -Device RP2040_M0_0 -If SWD -Speed 4000 -autoconnect 1 -CommanderScript tests/hil/fault_tick.jlink
# 5) 导出的 LA 抓包用纯 node 分析（不需要工具链）
node tests/hil/analyze_fault_capture.js <抓包.csv>
# 6) 复位回正常态
"<JLink.exe>" -Device RP2040_M0_0 -If SWD -Speed 4000 -autoconnect 1 -CommanderScript tests/hil/reset.jlink
```

## 期望读数（两种注入方式相同）

| 项 | 注入前 | 停泊后 |
|---|---|---|
| `GPIO_IN`(0xd0000004) 的 GP8（HV_EN）| 1 | **0** |
| `GPIO_IN` 的 GP9（FL_EN）| 0 | **1** |
| `GPIO_IN` 的 GP7（BK）| PWM 交替 | **恒高**（全消隐）|
| `GPIO_IN` 的 GP5/GP4/GP6（CLKg/LAT/SIg）| 扫描活动 | **1 / 0 / 0** |
| pio：`PIO0_CTRL`(0x50200000) SM 使能 | 0x01 | **0x00** |
| `SIO_OUT`(0xd0000010) bit25（GP25 LED）| 心跳闪烁 | **1（常亮）** |
| `_fatal`（`gDisplay + 偏移`）| 0 | **1** |

**`_fatal` 的地址**随构建变化：从 `nm` 拿 `gDisplay`，偏移从 `task()` 反汇编取
（`movs r3,#163; lsls r3,#5` ⇒ 偏移 5216 = 0x1460），即 `gDisplay + 0x1460`。

## 两个坑（都踩过）

1. **LA 触发被上电时序抢走**：脚本开头的 `reset` 之后，上电时序第①步也是 `HVEN=0` ⇒ 触发落在开机、
   抓到的 `FLEN` 脉冲（+4 ms 起到 +25 ms 落）是**灯丝驱动自检脉冲**、"扫描恢复"是预热结束。
   ⇒ 测试前先让板子启动完成，再布防，且**注入脚本里不要 reset**。
2. **tick 定时器类故障下停泊会卡在 `sleep_ms(10)`**（**v1.5.2 已修复**：改成直接读硬件计时器的忙等，不再依赖定时器中断；以下是旧版本的现象记录）：`emergencyOff()` 的这句延时依赖定时器中断。
   安全三动作（HVEN/FLEN/BK）已立即生效，但 `engineStop()/_fatal/LED` 要等下一个中断事件 ⇒
   `fault_tick.jlink` 里用两次 halt/resume 制造 WFE 唤醒让它走完（v1.5.2 起不需要了）。

## 实测记录（2026-10-17）

* pio：`NVIC_ICER0=bit11` ⇒ 3 s 后 HV_EN=0 / FL_EN=1 / `PIO0_CTRL=0` / `_fatal=1` / **LED 常亮**；
* tick：`NVIC_ICER0=bit3` ⇒ +0.5 s HV_EN=0/FL_EN=1、+1.2 s `_fatal=1`+LED 常亮、+3 s 保持；
* LA 抓包（有效样本）：`Fault-PIO-dma-irq.csv`、`Fault-Tick-timer-irq.csv`
  —— HVEN↓ → FLEN↑（+0.4 µs，单次保持）→ BK 恒高；扫描在触发前（判停窗口）已停。
* 详细记录：开发仓库 `docs/08` §7、`docs/07`（B1）；LA 抓包本身不入库（单份 1.5–2.3 MB），需要时向维护者索取。