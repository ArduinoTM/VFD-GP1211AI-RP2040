# VFD-GP1211AI-RP2040

[![CI](https://github.com/ArduinoTM/VFD-GP1211AI-RP2040/actions/workflows/ci.yml/badge.svg)](https://github.com/ArduinoTM/VFD-GP1211AI-RP2040/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ArduinoTM/VFD-GP1211AI-RP2040?label=release)](https://github.com/ArduinoTM/VFD-GP1211AI-RP2040/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

把 [VFD-GP1211AI](https://github.com/Trigger-CN/VFD-GP1211AI)（GP1211AI / Noritake MN12864K 128×64 VFD 的
STM32F103 Arduino 驱动）**移植到 RP2040 / Raspberry Pi Pico（Pico SDK）**，并顺手修掉了原驱动的一批
规格合规与健壮性问题。仓库内附带可在 PC 上运行的测试（**不需要 Pico SDK**）。

```
128×64 VFD（GP1211AI，无显示控制器）
        │  384bit 阳极链 + 48bit 栅极链 + 45V VDD2 + 2.9Vac 灯丝
   RP2040（Pico SDK, C++17, CMake）—— 两种扫描引擎，编译期二选一
        ├─ tick（默认）: SPI0 4.5 MHz（实测 4.464 MHz = 125 MHz/28）MODE3 → CLKa/SIa + DMA + repeating_timer 189 µs
        └─ pio         : PIO 状态机产生 CLKa/SIa/CLKg/LAT/SIg + 整帧 DMA（CPU 只在帧边界介入）
    公共: PWM（一周期 = 一扫描周期）→ BK 消隐/调光；双缓冲发布；扫描心跳看护
    可选: 双核分工（-DVFD_DUAL_CORE=ON，默认关）—— core0 = 扫描时序 + 帧发布 + 看护，
          core1 = SSD1306 数据面（排水/解码/渲染）；核间 2 槽 SPSC 双缓冲 + 序号握手，宁丢帧不撕裂
    另: 可把本机当"一片 SSD1306 OLED"用（4 线 SPI 从机 + 命令/GDDRAM 行为模拟）
```

* 帧缓冲 1 KB + 双缓冲发送缓冲 2×2064 B；tick 引擎中断 CPU ≈2%，PIO 引擎 ≈0.06%
  （原驱动 ≈24% 忙等）。**双核可把 SSD1306 数据面整个搬到 core1**，core0 只剩时序与发布；
  占用率可用诊断里的 4 个子项（`core0_irq`/`core0_work` | `core1_drain`/`core1_render`）直接量出来
* **性能（2026-10-14 实测，QEMU/Cortex-M0 + `-icount` 指令计数）**：帧缓冲 → 移位链的位重排改为
  **表驱动 gather**（每帧 **124.4k → 43.9k 指令**，界面型内容；tick 的整帧位反序折进打包，
  再省 27k 指令/帧）；SSD1306 模拟的渲染行映射改成"每次渲染只算 64 项"
  （滚动中 **4.90 → 1.17 ms/次**）。单核上机（诊断开、屏幕横向+纵向滚动）实测：
  `render` **30% → 6%**、>4 ms 的长排水间隔 **51 → 0**、四项忙时合计 75% → 61%
* 完全符合手册关键条款：fCLK ≤ 5 MHz、CLK 空闲高（Note 7①）、
  传输期间 BK 不变（Note 7②③④）、桁间消隐 ≥5 µs（Note 16）——
  两个引擎**四档亮度抓包验收全绿**（`0x00/0x80/0xA0/0xFF`：`analyze_la_csv.js --compliance` 16/16 ✅、
  `check_seed_window.js` exit 0），黄金样本入库 `tests/captures/`（没有硬件也能复跑门禁）
* **亮度 20%–80%（38–151 µs）**：搬运用「落点两选一」满足 Note 7② —— 高亮度整段落在**点亮**窗口内
  （手册 Figure 5 形态）、低亮度整段落在**消隐**窗口内；映射与边界只有 `src/vfd_pio_seed_timing.h` 一份，
  自洽性由编译期 `static_assert` 互锁
* 自带**扫描心跳看护**：手册 Note 14 明确"栅极扫描停止可能永久损坏屏"，
  停摆会自动恢复，连续失败则**停泊**（先关高压 + 关断灯丝驱动 + BK 全消隐 + 停扫描，板载 LED 常亮）。
  停泊的四项动作**已在实机确认**（2026-10-17：J-Link 读引脚实际电平 + 逻辑分析仪抓包 —— HVEN↓ 先于 FLEN↑、
  BK 转恒高、`engineStop()` 后扫描停、且保持不恢复；**两个引擎**都另用真实故障注入（pio 关 DMA 中断、tick 关定时器中断）走完整 fatal 路径，板载 LED 常亮亦已确认）
* **SSD1306 行为模拟**：对端 MCU 用现成 SSD1306 库（Adafruit_SSD1306 / u8g2…）
  经 4 线 SPI 直接把画面发过来即可；支持显示/反显/全亮/对比度（→亮度）、页/水平/垂直寻址、
  **滚动（`0x26/0x27/0x29/0x2A` + `0x2E/0x2F`）**；**内置测试图像默认不渲染**，只有把
  `VFD_PIN_TEST_MODE`（默认 GP16）在上电时拉低才进入测试模式

### 已实机验证的结论（2026-10-08，tick 与 pio 两引擎均确认）

| 项 | 结论（已固化进实现，不再是编译开关） | 代码位置 |
|---|---|---|
| LAT 极性 | **空闲低、锁存打正脉冲**（驱动电路无反向器；手册 Figure 3 画的就是正脉冲） | `.pio` 的 `set` 组 / tick 的 `vfdLatPulse()` |
| CLKg 极性 | **空闲高**；低→高脉冲的上升沿 = 栅极链前进一位 | 同上 |
| 扫描边界 | **四步**：① CLKg 拉低 → ② CLKg↑ **前进** → ③ LAT↑ **锁存** → ④ LAT↓ **释放**（先前进再锁存，否则画面整体平移 3 列） | `src/vfd_scan.pio` / tick 引擎 |
| 帧末播种 | 由 **CPU** 在**整帧最后一次扫描**做（SIg = 1,1,0,0,0 五个 CLKg 脉冲），**全程保持 LAT 低**（否则链搬运可见 = 边缘闪烁/线框抖动），并**等点亮起点之后**再发（pio 在帧末 DMA 完成中断里、tick 在一次性闹钟里）—— 位置在 A42 之后、A0 之前（= 帧首扫描边界之前，逐位等价）；**末步必须把 CLKg 留在高**（pio 曾把末步写成 CLKg 低 ⇒ 每帧 1 次 Note 7③） | `Rp2040Platform::seedGrid()` / `onFrameDmaDone()` / tick 的 `triggerThunk()` |
| 带内 3 列槽序 | **原驱动约定**：`t 偶 → off 0,2,4`、`t 奇 → off 5,3,1` | `packScan()` |
| 扫描相位 | **−1**（边界锁存的是上一扫描移入的数据 ⇒ 数据侧提前一个扫描发） | `wirePrepareFrame()` |
| T43 | 手册 Note 12：该时序**只能用 a,b** ⇒ `off 2 / off 4`、`k` 整体 −2、只写两个槽（c,d,e,f 全 OFF） | `packScan()` 的 `t43` 分支 |
| 数据段 | 逐 bit 消费（`set y,2` + `set x,15` + `jmp x--` + `jmp y--`，程序 26/32）；**不要**再用"每扫描一个控制字 + `out x,32` 按字读 OSR"（OSR 初始 32 bit 会让流错位） | `src/vfd_scan.pio` |
| 两条 `wait` | 不可省（一次扫描只有 ≈88 µs，结束时 BK 仍为消隐高电平） | 同上 |

## 目录

```
├─ CMakeLists.txt              # Pico SDK 目标工程（-DVFD_SCAN_ENGINE=tick|pio）
├─ src/
│  ├─ vfd_scanpack.{h,cpp}     # 平台无关：帧缓冲 -> 43×48 字节位重排（纯函数）
│  ├─ vfd_gp1211ai.{h,cpp}     # 驱动层（继承 Adafruit_GFX）：绘图原语 + 双缓冲发布 + 看护
│  ├─ vfd_platform.h           # 硬件抽象接口（换 MCU 只实现它）
│  ├─ vfd_platform_rp2040.{h,cpp}       # 两引擎共用：引脚/上电/亮度 PWM/发布/看护
│  ├─ vfd_platform_rp2040_tick.cpp      # tick 引擎：repeating_timer + SPI + DMA
│  ├─ vfd_platform_rp2040_pio.cpp       # pio  引擎：PIO 状态机 + 整帧 DMA
│  ├─ vfd_scan.pio / .pio.h             # 扫描 PIO 程序（25 条指令 / SM 上限 32）与已生成头文件
│  ├─ ssd1306_emulator.{h,cpp}          # SSD1306 行为模拟核心（命令/GDDRAM/寻址/渲染，可宿主测试）
│  ├─ ssd1306_slave_rp2040.{h,cpp}      # 4 线 SPI 从机传输层（PIO + DMA 环形缓冲）
│  ├─ ssd1306_spi_slave.pio / .pio.h    # 从机 PIO 程序（15 条指令）与已生成头文件
│  ├─ demo_screens.{h,cpp}     # 测试模式用的内置图像（宿主机测试也会用）
│  ├─ main.cpp                 # 示例程序：按测试引脚选择"测试图像"或"SSD1306 模拟"
│  └─ compat/                  # Arduino 兼容层（让 Adafruit_GFX 在裸 Pico SDK 上编译）
├─ lib/Adafruit_GFX/           # vendor 的 Adafruit GFX v1.2.3（BSD，仅必需 4 文件）
├─ tests/                      # 宿主机测试（g++，无需嵌入式工具链）
│  ├─ run_host_tests.ps1       # 一键跑：6 个测试程序（343 项 + 核间握手 79102 项）+ SDK 语法桩 + 6 个 .ino 语法
│  ├─ reference_pack.*         # 原驱动 display() 的 1:1 转录（位重排对照用）
│  ├─ host_syntax/stub/        # Pico SDK 形状桩（让目标机源码能在 PC 上做 -fsyntax-only）
│  ├─ host_syntax/stub_arduino/ # Arduino/Adafruit/u8g2 形状桩（给 examples/ 的 .ino 做语法检查）
│  ├─ tools/                   # 逻辑分析仪抓包判读脚本（node：渲染/合规/位序反解/播种窗口）
│  └─ captures/                # 黄金抓包样本（.csv.gz；无硬件也能复跑 --compliance 门禁）
├─ examples/                   # 主机侧样例 sketch（Pico 2 / Due；u8g2、Adafruit、接线探针）
├─ hardware/                   # 接线与电平注意事项（wiring.md）+ 预留（原理图/实物照片）
└─ docs/综合技术报告.md        # 精简技术报告（纯 Markdown）
```

## 快速开始

### 1) 宿主机测试（1 分钟，不需要任何嵌入式工具链）

```powershell
powershell -File tests\run_host_tests.ps1      # 需要 g++/clang++（6 个程序：79+122+46+53+43 = 343 项，另加核间握手回归 79102 项）
```

跑的是**目标机同一份驱动源码**（`src/`），用一个模拟扫描引擎的 `MockPlatform`：
位重排与原驱动 1:1 转录逐位对比、双射校验、双缓冲无撕裂回归、看护逻辑、示例画面 ASCII 预览；
`ssd1306_host_tests.exe`（**122 项**）则覆盖 SSD1306 模拟核心（标准初始化+整屏写入 ⇒ 渲染与主机图像逐位一致、
段/COM 镜像、三种寻址模式与窗口回绕、显示开关/全亮/反显/对比度、未知命令与跨调用拆分的参数、滚动：
参数个数按命令不同、垂直只上移、`0xA3` 滚动区、`0x2E` 停止后垂直复位而水平保留，以及
**Adafruit_SSD1306 等主机库的真实滚动字节流回放**）；
另有 `pio_seed_timing`（46 项：PIO 程序结构 + 边界四步 + 扫描相位模型 + 两个引擎的落点/亮度映射模型 + 帧末播种等价性 + 实测复核断言）、
`ssd1306_pio_host_tests`（53 项：从机 PIO 线上格式）、`master_phases_host_tests`（43 项：主控相位序列回放）
与 `dualcore_handoff`（**79102 项**：双核核间 2 槽 SPSC 协议的定种子随机交错仿真（≈75k 次不变量求值 + 槽位映射全循环） —— 丢帧守卫、槽位轮换、最新帧选择）。

**逻辑分析仪门禁**（第三方无硬件也可复跑）：`tests/tools/analyze_la_csv.js <csv> --compliance` 逐条核对手册
电气特性与 Note 3/7①②③④/10-11/14/16，**任一条不合格或测不到都返回非 0**；`tests/tools/check_seed_window.js <csv>`
核对帧末播种落点（不被 BK 边沿打断、LAT 全程低、到帧首锁存有余量）。
`tests/captures/` 里有 pio 与 tick 两份**全绿黄金样本**（3 帧裁剪 + gzip），可直接拿来跑通这两条命令。

脚本还会用**Pico SDK API 形状桩**（`tests/host_syntax/stub/`）对目标机源码
（`vfd_platform_rp2040.cpp` / `_tick.cpp` / `_pio.cpp`、`ssd1306_emulator.cpp`、
`ssd1306_slave_rp2040.cpp`、`main.cpp`，分别以 `-DVFD_SCAN_ENGINE_PIO=0/1`、
`-DVFD_DEBUG_DIAG=0/1`、`-DVFD_DUAL_CORE=0/1` 的组合编译多遍，共 17 个 SDK 目标 + 6 个 `.ino`）
做 `-fsyntax-only` 语法/类型检查 —— 在没装 pico-sdk 与 arm-none-eabi-gcc 的机器上
也能抓出语法、类型、成员名错误（它不能替代真实固件构建）。

> **Windows/MinGW 用户注意**：本地跑 `tests\run_host_tests.ps1` 会看到 **1 条** `-Wattributes` 对齐告警
> （`requested alignment 16384 is larger than 8192`）—— 这是 PE/COFF 目标格式的 8 KB 对齐上限，**不是缺陷**
> （`alignas(16384)` 是刻意的：环形 DMA 缓冲要按大小对齐；arm-none-eabi / Linux 的 ELF 目标没有这条限制，
> 所以 CI 上的"0 警"成立）。

> **文档数字一致性门禁**：`run_host_tests.ps1` 跑完会自动执行 `tests/tools/check_doc_numbers.js`，
> 把 pio 指令数（取自 `vfd_scan.pio.h`）、宿主测试项数（取自实测输出）、黄金样本清单与本仓库文档比对，
> 不一致即失败 —— 防止 README / 报告 / CI 工作流里的数字与代码脱钩。门禁自带 `--selftest` 回归用例
> （含「带日期的错误数字」这类假阴性场景），随宿主测试一起跑。
>
> **产物级核对（第三方无需嵌入式工具链）**：`tests/elf/` 里入库了三个构建（pio v1.4.0 / v1.5.0、
> tick v1.5.2）的 `objdump`/`nm` 输出（`.txt.gz`/`.nm.gz`），于是
> `node tests/tools/check_park_gpio.js`（fatal 停泊 + 状态 LED 的 SIO 写入路径/顺序）与
> `node tests/tools/compare_elf_functions.js`（pio v1.4.0 → v1.5.0 行为中性对照：581/601 逐指令相同、
> watch 清单全同）**只要 node 就能复跑**，两条都已接进 `run_host_tests.ps1` 第 ⑨ 步随 CI 执行。
>
> **上机验证（HIL）**：`tests/hil/` 归档了 **fatal 停泊**的上机验证流程与脚本 —— 两种"只写寄存器"的真实故障注入
> （pio 关 DMA 中断 / tick 关定时器中断）+ J-Link 读回判据 + LA 抓包分析脚本（纯 node），需 J-Link 与**未接面板**的测试板。

### 2) RP2040 固件

```bash
export PICO_SDK_PATH=/path/to/pico-sdk     # Windows: $env:PICO_SDK_PATH="C:\pico\pico-sdk"

# 默认：tick 引擎（定时器 + SPI + DMA）+ SSD1306 行为模拟
cmake -S . -B build -G Ninja -DPICO_BOARD=pico
cmake --build build -j
# build/vfd_gp1211ai_demo.uf2  -> 按住 BOOTSEL 插入 USB，拖入即可

# 可选：PIO 扫描引擎（扫描时序完全由硬件状态机产生）
cmake -S . -B build-pio -G Ninja -DPICO_BOARD=pico -DVFD_SCAN_ENGINE=pio
cmake --build build-pio -j

# 可选：双核分工（core0 = 时序 + 帧发布 + 看护；core1 = SSD1306 数据面）
cmake -S . -B build-dual -G Ninja -DPICO_BOARD=pico -DVFD_DUAL_CORE=ON
cmake --build build-dual -j
```

> **本机已验证过的完整命令**（工具不在 PATH 时把这两条路径显式带上即可；当前固件就是这样构建的）：
>
> ```powershell
> $tools = '<你的工具链目录>'            # 例如 pico-sdk / xpack gcc / picotool 所在目录
> $env:PATH = "$tools\xpack-arm-none-eabi-gcc-13.2.1-1.1\bin;$env:PATH"   # 把 ninja 所在目录也加上
> cmake -S . -B build-rp2040-pio -G Ninja "-DCMAKE_MAKE_PROGRAM=C:\Program Files\Meson\ninja.EXE" `
>       "-DPICO_SDK_PATH=$tools/pico-sdk" -DPICO_BOARD=pico -DPICO_NO_PICOTOOL=1 `
>       -DCMAKE_BUILD_TYPE=Release -DVFD_SCAN_ENGINE=pio
> cmake --build build-rp2040-pio
> # 生成 uf2（SDK 内建 picotool 被关掉时用独立 picotool）
> & "$tools\picotool\picotool\picotool.exe" uf2 convert `
>     build-rp2040-pio\vfd_gp1211ai_demo.elf build-rp2040-pio\vfd_gp1211ai_demo.uf2
> ```
>
> 产物（Release；**诊断关 / 诊断开**；2026-10-17 六个配置全部 `-Clean` 重建：**0 告警 0 错误**）：
> `build-rp2040/vfd_gp1211ai_demo.uf2`（tick 单核，**135 / 152 KB**，text 69012 / 77820 B、bss 33596 / 33648 B）与
> `build-rp2040-pio/vfd_gp1211ai_demo.uf2`（pio 单核，**135 / 152 KB**，text 68892 / 77572 B、bss 35628 / 35680 B）；
> 双核（`-DVFD_DUAL_CORE=ON`，诊断开）tick **145.5 KB**（text 74404 / bss 36720）、pio **145 KB**（text 74004 / bss 38752）。

### 3) 一键构建脚本 `build.ps1`（不想记参数就用它）

脚本会**自动定位工具链**（arm-none-eabi-gcc / ninja / Pico SDK / picotool / pioasm，也认 `PICO_SDK_PATH` 等环境变量），
设好环境变量后**逐项问你要编哪个模式**，再配置、编译、转出 `.uf2`，最后打印体积、产物路径和"板上启动横幅应为"的指纹。

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1          # 交互式：引擎 / 诊断 / 类型 / 双核 / 目录名
powershell -ExecutionPolicy Bypass -File build.ps1 -Yes     # 全默认：pio + 诊断 off + Release + 单核
powershell -ExecutionPolicy Bypass -File build.ps1 -Engine both -Diag on -Yes        # 两引擎各出一个 uf2（诊断只走 UART）
powershell -ExecutionPolicy Bypass -File build.ps1 -DualCore -Diag on -DiagUsb -Yes  # 双核 + 诊断同时走 USB 与 UART
powershell -ExecutionPolicy Bypass -File build.ps1 -Engine pio -Dir build-x -Yes     # 指定构建目录
powershell -ExecutionPolicy Bypass -File build.ps1 -Test -Clean -Yes                 # 先跑宿主测试、删目录重来
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean -Yes -CMakeVar VFD_TICK_RX_TOPUP=0  # 额外透传 CMake 变量（可逗号分隔多个）
powershell -ExecutionPolicy Bypass -File build.ps1 -CleanOnly                        # 只清理本次会用到的构建目录（不编译、不提问）
powershell -ExecutionPolicy Bypass -File build.ps1 -CleanAll                         # 只清理所有 build-* 与 tests\build（不编译、不提问）
```

| 参数 | 取值 | 含义 |
|---|---|---|
| `-Engine` | `pio` / `tick` / `both` | 扫描引擎 |
| `-Diag` | `on` / `off` | 逐秒诊断。**默认 `off`**；`on` 时诊断**只走 UART**（GP0/GP1），uf2 大 ~17 KB（实测 135 → 152 KB） |
| `-DiagUsb` | 开关 | 诊断**是否也走 USB**（默认只走 UART）。USB stdio 的写会阻塞主循环，排故时不想额外接 UART 再开它 |
| `-DualCore` | 开关 | **双核分工**：core0 = 扫描时序 + 帧发布 + 看护，core1 = SSD1306 数据面。默认关 = 与单核逐行为一致 |
| `-Type` | `Release` / `Debug` | 构建类型（只影响优化级别/调试信息，**与诊断开关无关**） |
| `-Dir` | 目录名 | 构建目录（默认 `build-rp2040-pio` / `build-rp2040`；后缀见下） |
| `-Test` | 开关 | 编译前先跑宿主测试 |
| `-Clean` | 开关 | 先删本次的构建目录，**再**编译（清理阶段不提问） |
| `-CleanOnly` | 开关 | **只清理**：删掉本次会用到的构建目录后退出（不编译、不提问） |
| `-CleanAll` | 开关 | **只清理**：删掉仓库内所有 `build-*` 与 `tests\build` 后退出（不编译、不提问） |
| `-CMakeVar` | `NAME=VALUE[,NAME=VALUE…]` | 额外透传 CMake 变量（自动补 `-D`；省略 `-Dir` 时构建目录名带 `-custom` 后缀） |
| `-PioRegen` / `-Yes` / `-Help` | 开关 | 用 pioasm 重新生成 `.pio.h` / 不提问 / 打印详细帮助 |

构建目录后缀：**`-diagon`**（诊断开）/ 无后缀（诊断关，默认）/ **`-diagusb`**（诊断也走 USB）/ **`-dual`**（双核）/ **`-debug`**（Debug 类型）。

> **屏幕时序相关的调参开关已全部移除**（2026-10-08 实机定标后固化进实现，见下方"已实机验证"）：
> LAT 极性、带内 3 列槽序、扫描相位都不是编译选项了，改它们要动源码（`src/vfd_scanpack.h` 顶部有完整说明）。

> `.pio` 比生成的 `.pio.h` 新时脚本会**提醒**（这类"改了源却编了旧头"的坑踩过），需要时加 `-PioRegen`。

| CMake 选项 | 默认 | 说明 |
|---|---|---|
| `VFD_SCAN_ENGINE` | `tick` | `tick` = 定时器+SPI+DMA；`pio` = PIO 状态机+DMA |
| `VFD_DUAL_CORE` | OFF | **双核分工**（core0 时序+发布，core1 SSD1306 数据面）；关 = 单核行为 |
| `VFD_DEBUG_DIAG` | `off` | 逐秒诊断输出（`on`/`off`）。与 `CMAKE_BUILD_TYPE=Debug` **无关** |
| `VFD_DEBUG_DIAG_USB` | `off` | 诊断是否也走 USB；默认只走 UART（`uart_write_blocking`），不碰 USB |
| `VFD_PIO_CLK_HZ` | 4500000 | PIO 引擎的 CLKa 频率（须 ≤5 MHz；SM 时钟 = 2×） |
| `VFD_PIN_TEST_MODE` | 16 | **测试模式选择**：上电该脚为低 → 渲染内置测试图像；默认高 → 只做 SSD1306 从机 |
| `VFD_EMU_PIN_SCK/MOSI/DC/CS/RESET` | 11/12/13/14/15 | SSD1306 从机引脚（`DC` 必须 = `MOSI+1`；`RESET=255` 表示不接） |
| `VFD_EMU_PIO_INST` / `VFD_EMU_PIO_SM` | 1 / 0 | 模拟器用的 PIO 块/状态机（须与扫描引擎不同块） |
| `VFD_EMU_DMA_CH` | 2 | 模拟器 DMA 通道（勿与扫描引擎冲突） |
| `VFD_TICK_RX_DMA_COUNT` | `0xFFFFFFFF` | tick：RX 排空 DMA 的计数（跑满即补计数 `rearm`；按 48 B/189 µs 折算默认约 2.35 h 一轮） |
| `VFD_TICK_RX_TOPUP` | 1 | tick：RX DMA 补计数开关（`0` = 关，A/B 实验用） |
| `VFD_TICK_SERVICE_DRAIN` | 1 | tick：`service()` 里用 CPU 顺带排空 SPI RX FIFO（`0` = 关，A/B 实验用） |
| `VFD_PIO_REGENERATE` | OFF | 用 pioasm 重新生成 `src/*.pio.h`（改了 `.pio` 才需要） |
| `VFD_PIN_*` / `VFD_DMA_*` | 见下表 | 覆盖默认接线/通道 |
| `VFD_STDIO_USB` / `VFD_STDIO_UART` | ON | stdio 开关（无 tinyusb 子模块时关掉 USB） |

引脚覆盖：`-DVFD_PIN_BK=15 -DVFD_PIN_LAT=4 ...`（默认接线见下表）。
PIO 引擎额外要求 `LAT/CLKg/SIg` 三者**引脚连续**（默认 GP4/5/6），否则 `engineError()` 会置位。

#### 本仓库已做过的真实构建验证

| 项 | tick 引擎（默认） | pio 引擎 |
|---|---|---|
| pico-sdk / 工具链 | 2.1.0（含 `lib/tinyusb`）/ xPack GNU Arm Embedded GCC 13.2.1 + Ninja | 同 |
| 配置 | `-DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release` | 同上 + `-DVFD_SCAN_ENGINE=pio` |
| 诊断输出 | `-DVFD_DEBUG_DIAG=off`（默认）。`on` = 逐秒打印 SSD1306 状态 / CPU 占用 / 帧抖动 / pinmon / slave-dbg / pc_hist / waits / `gapmax`(排水间隔) / `SCAN`(RX DMA)，**默认只走 UART**（`-DVFD_DEBUG_DIAG_USB=on` 才同时走 USB）；`off` 只留启动横幅与告警 | 同左（两引擎共用同一份 `main.cpp`）|
| Flash / RAM | 诊断 off `text 69012 B` / `bss 33596 B`；诊断 on `text 77820 B` / `bss 33648 B` | 诊断 off `text 68892 B` / `bss 35628 B`；诊断 on `text 77572 B` / `bss 35680 B` |
| 中断放置 | `scanTimerThunk`（含内联的 `scanTick`）位于 **RAM 0x200000d4，292 B** | `dmaIrqThunk`（含内联的 `onFrameDmaDone`/排程）位于 **RAM 0x20000150，276 B** |
| PIO 程序 | 从机 **15/32** 条指令（`ssd1306_spi_slave.pio`，含滚动） | 扫描 **26/32**（`vfd_scan.pio`）+ 从机 15/32，运行时均经 `pio_can_add_program()` 校验 |
| 产物 | `build-rp2040/…uf2`（诊断关 **135 KB** / 诊断开 152 KB） | `build-rp2040-pio/…uf2`（诊断关 **135 KB** / 诊断开 152 KB） |
| 双核（`-DVFD_DUAL_CORE=ON`，诊断开） | `build-rp2040-diagon-dual/…uf2` **145.5 KB**（`text 74404 B` / `bss 36720 B`） | `build-rp2040-pio-diagon-dual/…uf2` **145 KB**（`text 74004 B` / `bss 38752 B`） |

如果本机没有安装 picotool，可先 `-DPICO_NO_PICOTOOL=1` 配置（此时不产出 uf2），
再用任意独立 picotool 转换：`picotool uf2 convert <elf> <uf2>`。

构建过程中发现并修掉的几个真实陷阱：
* SDK 里**没有** `pwm_channel_t`，它是 `enum pwm_chan { PWM_CHAN_A, PWM_CHAN_B }`，
  而 `pwm_gpio_to_channel()` 返回 `uint`；
* PIO 相关头文件一旦启用 `-O3`，`scanTick()` 会被内联进调用它的 thunk ——
  若只给 `scanTick()` 加 `__not_in_flash_func`，中断代码仍会留在 Flash；两个 thunk 也要一起标注；
* pioasm 不支持 `.equ`/`|` 运算符，且 `set` 立即数上限 31（48 字节要拆成 3×16 循环）；
* SSD1306 从机的 DC 必须紧邻 MOSI（`in pins,2` 一次采 {MOSI,DC}），代码会校验。

| Pico | GP2 | GP3 | GP4 | GP5 | GP6 | GP7 | GP8 | GP9 | GP10 | GP11 | GP12 | GP13 | GP14 | GP15 | GP16 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 用途 | CLKA | SIA | LAT | CLKG | SIG | BK | HVEN | FLEN | —（保留） | 主机 SCK | 主机 MOSI | 主机 DC | 主机 CS | 主机 RESET | 测试模式¹ |

¹ 上电为低 → 渲染内置测试图像；高/悬空 → 只做 SSD1306 从机（默认）。

（另：**GP25 = 板载状态 LED**，不接驱动板 —— 正常工作时闪烁（心跳）、fatal 停泊后常亮；
`-DVFD_PIN_LED=n` 可改，`255` = 关闭。Pico W 等没有该宏的板子自动不驱动。）
（GP10 早期用作 PIO 的"帧末播种请求"握手脚，现播种由 CPU 在帧末直接翻引脚，该脚已不再使用。）

> ⚠️ 驱动板有 **45 V（VDD2）** 与 **2.9 Vac 灯丝**；`FLEN` 高电平是**关断**灯丝驱动。
> 接线、上电前请确认驱动板状态，调试时建议先断开高压只验证逻辑。

### 3) 最小用法

```cpp
#include "vfd_gp1211ai.h"
#include "vfd_platform_rp2040.h"

vfd::Rp2040Platform platform(vfd::defaultRp2040Config());
VFD_GP1211AI display(platform);

display.begin();               // 平台 init（含扫描引擎）+ 灯丝预热 + 上高压
display.setBrightness(200);    // 0..255

display.clearDisplay();
display.setTextColor(WHITE);
display.setCursor(0, 0);
display.print(F("hello, VFD"));
display.display();             // 重排 + 发布

for (;;) {
    display.task();            // 必须周期调用：扫描看护 + 杂务
    // ... 画下一帧 ...
    display.display();
}
```

## 与原驱动的差异（要点）

| 项 | 原驱动（STM32F103） | 本移植（RP2040） |
|---|---|---|
| 阳极时钟 | `setClock(12e6)` → 实跑 **9 MHz**（超手册 5 MHz） | 目标 **4.5 MHz**（实测 4.464 MHz），MODE3（空闲高） |
| 阳极数据搬运 | ISR 阻塞式 `SPI.write`，≈24% CPU | **DMA**，ISR 只翻转引脚 + 触发，≈1~2% CPU |
| 调光 | BK 94 kHz 自由 PWM（违反 Note 7②） | "消隐窗口"：一 PWM 周期 = 一扫描周期 |
| 桁间消隐 | 无显式实现（Note 16） | ≈100 µs（≫ 5 µs） |
| 帧缓冲/撕裂 | `display()` 与 ISR 共用一块发送缓冲 | **双缓冲 + 帧边界握手**，任意调用节奏不撕裂 |
| 灯丝预热 | 100 ms | 400 ms（可配） |
| 扫描保护 | 无 | 心跳看护 → 自动恢复 → 连续失败**停泊**（关高压 + 关灯丝驱动 + LED 常亮） |
| 颜色 ≥3 | 静默丢弃（默认文字色画不出字） | 归一化为 WHITE |
| `invertDisplay()` | 无效（基类空实现） | 生效（重排阶段按像素取反） |
| `fillScreen(INVERSE)` | 变成白色 | 整体取反（与 `fillRect(INVERSE)` 一致） |
| `setRotation()` | 会越界写坏 `_sendBuffer` | 拒绝非 0 值 |
| 列 128 越界读 | 依赖对象内存布局的 UB | 显式跳过（该位本就不会点亮） |
| 可移植性 | 驱动里写死 Arduino/STM32 | 算法层 + 驱动层 + 平台层分离 |

* [`docs/综合技术报告.md`](docs/综合技术报告.md) —— **精简技术报告**（纯 Markdown，约 1/6 篇幅）：
  项目概述、硬件与手册要点、软件架构、扫描时序的实机定标结论、SSD1306 模拟支持面、
  与原驱动的差异、验证方法、已知取舍。

> 源码注释里出现的 `docs/NN-*.md`（如 `docs/13`）指向**开发仓库的完整分册文档集**（原驱动不足分析、
> 移植说明、PIO 引擎、实测定标、SSD1306 行为模拟、排故报告等）。**本仓库只随附上面这份精简报告，
> 分册不在本仓库内** —— 需要细节时请以源码注释 + 本报告为准，或向维护者索取分册。

## 当一片 SSD1306 OLED 用（4 线 SPI 从机）

默认模式下本机不渲染任何内置测试图像，而是等主机通过 4 线 SPI 送画面
（主机侧代码与驱动真实 OLED 完全一样，无需改动）：

```cpp
// 主机（例如另一块 Arduino）：SCK→GP11, MOSI→GP12, DC→GP13, CS→GP14, 共地
// ⚠️ bitrate 必须显式给 4 MHz：Adafruit_SSD1306 的默认值是 8 MHz（见下方要点）
Adafruit_SSD1306 oled(128, 64, &SPI, /*dc=*/9, /*res=*/-1, /*cs=*/10, /*bitrate=*/4000000);
oled.begin(SSD1306_SWITCHCAPVCC, 0);
oled.clearDisplay();
oled.setTextSize(2);
oled.setTextColor(SSD1306_WHITE);
oled.setCursor(0, 0);
oled.println(F("VFD as SSD1306"));
oled.display();      // 调一次即可，本机持续显示
```

要点：

* 主机 SPI 用**模式 0**（CPOL=0, CPHA=0）；`DC` 必须接在 **MOSI 的下一个 GPIO**（GP12→GP13）；
* 支持页/水平/垂直寻址、列页窗口、显示开关、全亮、反显、`0x81` 对比度（→ VFD 亮度）、RESET、
  **滚动**（`0x26/0x27` 水平右/左 = 6 个参数、`0x29/0x2A` 垂直+水平 = **5 个参数**、`0x2E/0x2F` 停止/启动、
  `0xA3` 垂直滚动区域；间隔按规格书帧数表，1 帧折算 8 ms）；
  水平滚动**改写 GDDRAM**（`0x2E` 停止后位移保留），垂直滚动是**显示级上移**（`0x2E` 停止后上下自动复位、
  只能向上）——详见 `docs/综合技术报告.md` §5；
  未知命令忽略并计数；
* 段/COM 重映射按"标准模块"约定：库默认的 `0xA1`+`0xC8` 与主机缓冲**同向**（逐位一致）；
* 内置测试图像只在 `VFD_PIN_TEST_MODE`（GP16）上电为低时渲染，用于点亮/时序自检；
* **上电即抓取**：主机接收通路在 `main()` 一开始就启动（早于 440 ms 灯丝预热与启动横幅），
  预热也不阻塞 CPU ⇒ 复位后主机发来的初始化序列与首批画面不会丢；横幅里的
  `boot capture: N B received before this loop (overrun=… drop=…)` 可核对上电窗口接收量。
  主机侧仍建议**上电 ≥150 ms 后再发初始化**（或先把 RESET 拉低 ≥150 ms 再释放）—— RP2040
  自身启动的那几十毫秒无法接收任何字节，详见 `docs/综合技术报告.md` §5。
* **失步看护**：从机检测到硬件 RXSTALL（时钟/数据受扰导致 PIO 停摆）时立刻停 SM 与 DMA、
  丢弃本帧剩余字节并重新对齐（最多损失一帧，不会整屏错位），CS 抬起等待有 20 ms 兜底；
  诊断计数 `stallfix` / `stallforced` 正常恒 0。
* **主机时钟用 4 MHz**（`examples/` 下有 u8g2 与 Adafruit 两个可烧样例 + `wiring_probe_due_r4` 探针）：
  ⚠️ **`Adafruit_SSD1306` 的 `spiSettings` 默认 8 MHz，请显式给 4 MHz**（构造函数最后的 bitrate 参数，
  或 `SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0))`）。理由有三条：
  · 8 MHz 下 PIO 从机的字节边界余量只剩 ≈85 ns（4 MHz 是 210 ns），CPU 排水占用 >70%（4 MHz ≈35%）；
  · 更关键的是**帧结构**：`Adafruit_SSD1306::display()` 把"6 条窗口命令 + 1024 B 数据"压在**一次 CS 拉低**里
    （单事务最大 1030 B），而 u8g2 是每页一个事务（≤132 B）。本机唯一的失步恢复点是 CS 边界
    (`endTransaction()`，500 µs 去抖) ⇒ 同样一次位相位错位，Adafruit 毁**整帧**、u8g2 最多毁**一页**；
  · 4 MHz 对 83 fps 刷新绰绰有余。要试 8 MHz 请先读 `docs/综合技术报告.md` §8 的余量说明。
* **接线**：Arduino Due 的硬件 SPI **只在 2×3 ICSP 排针**（ICSP-3=SCK/GPIO76、ICSP-4=MOSI/GPIO75；
  数字口 11/12/13 不是 SPI）；Uno R4 Minima 是 **5 V**，直连超规格（需电平转换或先降速短时试）；
  `DC` 必须接在 MOSI 的下一个 GPIO。
* **诊断怎么读**（`-Diag on`，每秒一行）：启动横幅的 `fw: <编译时间> …` 唯一标识固件版本；
  `gapmax=<µs>(起点段→终点段,rx+NB,big=N)` 是"本窗口最长多久没把主机数据取走"，以及**当时主机是否在发**
  （`rx+0` ⇒ 没发，无害）。危险线 = 4094 B ÷ 主机字节率：**4 MHz 8.19 ms ｜ 8 MHz 4.09 ms**；
  实测 4.464 MHz 下修复后为 **2.79 ms**（= 一个 32 B 诊断分块的串口时间，`-Diag on` 的地板），
  `overrun/drop` 全程 0。

## 两种扫描引擎怎么选

| 场景 | 建议 |
|---|---|
| 一般使用、想省一个 PIO 资源、习惯用 SPI | **tick**（默认） |
| 系统里有高优先级中断/长临界区，希望扫描时序不被软件拖累 | **pio** |
| 需要 CLKa 频率精确可调、或以后要接第二块屏（PIO 可多 SM 并行） | **pio** |

两种引擎输出到屏上的时序效果一致（同样的 189 µs 扫描周期、同样的亮度映射与落点规则 —— 低亮度搬运落在消隐窗口、高亮度落在点亮窗口，仅 `lit = 96…98 µs` 这几档 tick 因边界更长而改走点亮落点），
差别只在"谁产生时序"：tick 靠 5.29 kHz 中断，pio 靠状态机 + 123 Hz 的换帧中断。

> **双核分工（`-DualCore`）与引擎选择正交**：它只决定"SSD1306 数据面跑在哪个核"，扫描引擎照旧二选一。
> 打开后 core0 = 扫描时序 + 帧发布 + 看护，core1 = SSD1306 排水/解码/渲染（核间 2 槽 SPSC 双缓冲 + 序号握手，
> 宁丢帧不撕裂；**默认关 = 与单核逐行为一致**，可一键回滚）。
> 占用率用诊断的 4 个子项量：单核 `core0: irq/drain/render/work`，双核 `core0: irq/work | core1: drain/render`。

## 已知取舍

* **BK 波形形态与手册样例不同（已知偏差，用户已确认保留）**：手册样例是「平时低 + 每个边界两个 ≥5 µs 窄脉冲」，
  本实现是「一段消隐 + 一段点亮」；**合规不受影响**（Note 7②③④、Note 16、刷新率均按真机抓包核对）。
* ~~**峰值亮度 ≈47%**：阳极数据在消隐期间搬运~~ → **2026-10-17 起亮度 20%–80%（38–151 µs）**：
  搬运用「落点两选一」（高亮度整段落在**点亮**窗口内 = 手册 Figure 5 形态；低亮度整段落在**消隐**窗口内）
  满足 Note 7②，自洽性由 `src/vfd_pio_seed_timing.h` 的编译期 `static_assert` 互锁（tick 低亮度落点上限
  95 µs、pio 98 µs —— 两者搬运/边界耗时不同）。80% 档整机实测 ≈2.5 W @5 V，已占手册电气包线 ≈89%，故不再上调。
* 仍是"整帧重排 + 双缓冲"模型（约 4 KB RAM），未改成"每次扫描现场生成 48 字节"。
* PIO 引擎的**数据通路仍依赖 CPU 换帧**：CPU 长时间不响应帧中断时 DMA 会停供，
  PIO 卡在 `out`（`engineStalled()` 可查）。要做到"CPU 完全脱离也不停扫"，
  下一步可把整帧 DMA 做成自循环控制块链（间接寻址 + 自触发）。
* PIO 引擎靠 BK 引脚的电平沿同步，因此亮度 0 时仍保留最小点亮窗口（选项 C 起 = **38 µs**，占空 20%，是产品要求的亮度下限）。
* SSD1306 模拟：只支持 SPI 模式 0；`0xA8` 多路复用比只记录；渲染有 12 ms 节流（最高 83 fps，
  足够呈现最快档滚动 16 ms/步，主机的多次局部刷新会合并显示）；滚动时间基准是折算值
  （1 帧 = 8 ms，非逐晶振复刻）；从机环形缓冲 **16 KB**（4096 字节容量），异常突发时会计数
  `overrunCount()` / `droppedCount()`、保留最新数据并自动重新对齐。

## 自动化（GitHub Actions）

| 工作流 | 触发 | 做什么 |
|---|---|---|
| [`ci.yml`](.github/workflows/ci.yml) | push 到 `main` / PR / 手动 | 跑宿主机测试（343 项 + 核间握手 79102 项 + SDK 语法桩 + 6 个 `.ino` 语法），并编译 **4 种固件组合**（tick / pio × 单核 / 双核），uf2 作为构建产物上传 |
| [`release.yml`](.github/workflows/release.yml) | 推 `v*` 标签 / 手动 | 先跑测试，再编译同样 4 种组合，然后自动创建 GitHub Release 并把 4 个 uf2 附上（`-tick` / `-pio` / `-tick-dual` / `-pio-dual`，发布说明自动生成）|

发版只需打标签：

```bash
git tag -a v1.0.0 -m "v1.0.0" && git push origin v1.0.0
```

徽章在本页顶部；点进去可看每次运行的分步日志与 uf2 产物。

## 许可

MIT（见 `LICENSE`）：驱动算法与位映射源自原项目（_VIFEXTech / Trigger-CN），本仓库为其 RP2040 移植。
`lib/Adafruit_GFX/` 为 Adafruit GFX v1.2.3（BSD，见 `lib/Adafruit_GFX/license.txt`）。
`tests/reference_pack.*` 是原驱动 `display()` 的 1:1 转录，仅用于测试对照。
