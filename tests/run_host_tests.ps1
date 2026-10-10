#requires -version 5
<#
  宿主机测试：用系统 g++ 编译 src/ + lib/Adafruit_GFX/ + tests/，在 PC 上验证
    ① 位重排 / 双缓冲 / 绘图原语 / 扫描看护（vfd_host_tests.exe）
    ② SSD1306 行为模拟核心（ssd1306_host_tests.exe）
    ④ SSD1306 从机 PIO 线上格式：wait 引脚语义 + 9 位字布局（ssd1306_pio_host_tests.exe）
    ⑤ 主控"完整测试"相位序列回放（master_phases_host_tests.exe）
  另外用"Pico SDK API 形状桩"对目标机源码做语法检查，
  用"Arduino/SPI/U8g2 形状桩"对 examples 下的主控测试程序（.ino）做语法检查。
  用法：powershell -File tests\run_host_tests.ps1
#>
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$outDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$cxx = if ($env:CXX) { $env:CXX } else { 'g++' }
if (-not (Get-Command $cxx -ErrorAction SilentlyContinue)) {
    throw "找不到 C++ 编译器 '$cxx'，请安装 g++/clang++ 或设置环境变量 CXX"
}

$commonInc = @(
    "-I$(Join-Path $root 'src')",
    "-I$(Join-Path $root 'src/compat')",
    "-I$(Join-Path $root 'lib/Adafruit_GFX')",
    "-I$PSScriptRoot"
)
# Pico SDK API 形状桩（给"只依赖 SDK 形状"的测试/语法检查用）
$stubInc = @(
    "-I$(Join-Path $PSScriptRoot 'host_syntax/stub')",
    "-I$(Join-Path $root 'src')",
    "-I$(Join-Path $root 'src/compat')",
    "-I$(Join-Path $root 'lib/Adafruit_GFX')"
)
$warnArgs = @('-std=c++17', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter')

function Build-And-Run([string]$name, [string[]]$sources, [string[]]$includes = $null) {
    $exe = Join-Path $outDir "$name.exe"
    $inc = $commonInc
    if ($includes) { $inc = $includes }
    $compileArgs = $warnArgs + $inc + $sources + @('-o', $exe)
    Write-Host "编译: $name" -ForegroundColor DarkGray
    & $cxx @compileArgs
    if ($LASTEXITCODE -ne 0) { throw "编译失败: $name (exit $LASTEXITCODE)" }

    Write-Host "运行: $exe`n" -ForegroundColor DarkGray
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$name 存在失败项 (exit $LASTEXITCODE)" }
}

# ---------------------------------------------------------------------------
# ① 驱动核心测试（位重排 / 双缓冲 / 绘图原语 / 扫描看护 / 示例画面）
# ---------------------------------------------------------------------------
Build-And-Run 'vfd_host_tests' @(
    (Join-Path $PSScriptRoot 'test_main.cpp'),
    (Join-Path $PSScriptRoot 'mock_platform.cpp'),
    (Join-Path $PSScriptRoot 'reference_pack.cpp'),
    (Join-Path $root 'src/vfd_scanpack.cpp'),
    (Join-Path $root 'src/vfd_gp1211ai.cpp'),
    (Join-Path $root 'src/demo_screens.cpp'),
    (Join-Path $root 'src/compat/Print.cpp'),
    (Join-Path $root 'lib/Adafruit_GFX/Adafruit_GFX.cpp')
)

# ---------------------------------------------------------------------------
# ② SSD1306 行为模拟测试（命令解析 / GDDRAM / 寻址模式 / 渲染）
# ---------------------------------------------------------------------------
Build-And-Run 'ssd1306_host_tests' @(
    (Join-Path $PSScriptRoot 'test_ssd1306.cpp'),
    (Join-Path $root 'src/ssd1306_emulator.cpp')
)

# ---------------------------------------------------------------------------
# ③ PIO 帧末播种请求的时序窗口模型（位置/容差；复现旧窗口漏播的现象）
# ---------------------------------------------------------------------------
Build-And-Run 'pio_seed_timing' @(
    (Join-Path $PSScriptRoot 'test_pio_seed_timing.cpp'),
    (Join-Path $root 'src/vfd_scanpack.cpp')
)

# ---------------------------------------------------------------------------
# ④ SSD1306 从机 PIO 的"线上格式"（wait 引脚语义 + 9 位字布局）
#    用真实 pioasm 生成头（src/ssd1306_spi_slave.pio.h）+ SDK 形状桩；
#    测的就是生产用的纯逻辑（src/ssd1306_pio_wire.h）。
# ---------------------------------------------------------------------------
Build-And-Run 'ssd1306_pio_host_tests' @(
    (Join-Path $PSScriptRoot 'test_ssd1306_pio.cpp')
) $stubInc

# ---------------------------------------------------------------------------
# ⑤ 主控相位序列回放（契约测试）
#    把 examples/pico2_full_test 的相位序列在 PC 模拟器上跑一遍，锁死每相位的期望 CRC 与状态
#    （主控程序改相位时要同步改它；也能立刻发现"水平寻址变空屏/亮度停 0"这类回归）
# ---------------------------------------------------------------------------
Build-And-Run 'master_phases_host_tests' @(
    (Join-Path $PSScriptRoot 'test_master_phases.cpp'),
    (Join-Path $root 'src/ssd1306_emulator.cpp')
)

# ---------------------------------------------------------------------------
# ⑤b 跨核帧缓冲握手（2 槽 SPSC，core1→core0 帧发布协议）纯逻辑不变量测试
#    测 src/dualcore_handoff.h（生产用的同一份头）：槽位映射 + 可写边界 +
#    "生产者永不覆盖消费者未读帧"（随机交错仿真）。
# ---------------------------------------------------------------------------
Build-And-Run 'dualcore_handoff' @(
    (Join-Path $PSScriptRoot 'test_dualcore_handoff.cpp')
)

# ---------------------------------------------------------------------------
# ⑥ 主控测试程序（examples 下的 .ino）语法检查
#    本机没有 arduino-cli，用 Arduino/SPI/U8g2 形状桩把 sketch 编译一遍，
#    提前抓 API/拼写错误；桩的签名对照真实核心与库，见 stub_arduino/Arduino.h 注释
# ---------------------------------------------------------------------------
$inoStub = @("-I$(Join-Path $PSScriptRoot 'host_syntax/stub_arduino')")
$inoTargets = @(
    'examples/pico2_full_test/pico2_full_test.ino',
    'examples/pico2_u8g2_test/pico2_u8g2_test.ino',
    'examples/pico2_bitbang_wiring_test/pico2_bitbang_wiring_test.ino',
    'examples/due_full_test/due_full_test.ino'              # Arduino Due 移植版（SPI 只在 ICSP 排针上）
)
foreach ($ino in $inoTargets) {
    Write-Host "语法检查(Arduino 桩): $ino" -ForegroundColor DarkGray
    & $cxx '-std=c++17' '-fsyntax-only' '-Wall' '-Wextra' '-Wno-unused-parameter' '-x' 'c++' @inoStub (Join-Path $root $ino)
    if ($LASTEXITCODE -ne 0) {
        throw "主控程序语法检查失败: $ino"
    }
}

# ---------------------------------------------------------------------------
# ⑦ 目标机源码语法/类型检查（SDK 形状桩；两种引擎 + 模拟器传输层都过一遍）
# ---------------------------------------------------------------------------
$syntaxTargets = @(
    @{ File = 'src/vfd_platform_rp2040.cpp'; Engine = '0' },
    @{ File = 'src/vfd_platform_rp2040_tick.cpp'; Engine = '0' },
    @{ File = 'src/vfd_platform_rp2040_pio.cpp'; Engine = '1' },
    # 打开诊断开关再来一遍：验证中断忙时打点（irqUs）那段代码也能编译
    @{ File = 'src/vfd_platform_rp2040_tick.cpp'; Engine = '0'; Def = '-DVFD_DEBUG_DIAG=1' },
    @{ File = 'src/vfd_platform_rp2040_pio.cpp'; Engine = '1'; Def = '-DVFD_DEBUG_DIAG=1' },
    @{ File = 'src/ssd1306_emulator.cpp'; Engine = '0' },
    @{ File = 'src/ssd1306_emulator.cpp'; Engine = '1' },
    @{ File = 'src/ssd1306_slave_rp2040.cpp'; Engine = '0' },
    @{ File = 'src/ssd1306_slave_rp2040.cpp'; Engine = '1' },
    @{ File = 'src/main.cpp'; Engine = '0'; Def = '-DVFD_DEBUG_DIAG=0' },
    @{ File = 'src/main.cpp'; Engine = '1'; Def = '-DVFD_DEBUG_DIAG=0' },
    @{ File = 'src/main.cpp'; Engine = '0'; Def = '-DVFD_DEBUG_DIAG=1' },
    @{ File = 'src/main.cpp'; Engine = '1'; Def = '-DVFD_DEBUG_DIAG=1' },
    @{ File = 'src/main.cpp'; Engine = '0'; Def = '-DVFD_DUAL_CORE=1'; Def2 = '-DVFD_DEBUG_DIAG=0' },
    @{ File = 'src/main.cpp'; Engine = '1'; Def = '-DVFD_DUAL_CORE=1'; Def2 = '-DVFD_DEBUG_DIAG=0' },
    @{ File = 'src/main.cpp'; Engine = '0'; Def = '-DVFD_DUAL_CORE=1'; Def2 = '-DVFD_DEBUG_DIAG=1' },
    @{ File = 'src/main.cpp'; Engine = '1'; Def = '-DVFD_DUAL_CORE=1'; Def2 = '-DVFD_DEBUG_DIAG=1' }
)
foreach ($t in $syntaxTargets) {
    $defs = @("-DVFD_SCAN_ENGINE_PIO=$($t.Engine)")
    if ($t.Def) { $defs += $t.Def }
    if ($t.Def2) { $defs += $t.Def2 }
    $label = ($defs -join ' ')
    Write-Host "语法检查(SDK 桩, $label): $($t.File)" -ForegroundColor DarkGray
    & $cxx '-std=c++17' '-fsyntax-only' '-Wall' '-Wextra' '-Wno-unused-parameter' @defs @stubInc (Join-Path $root $t.File)
    if ($LASTEXITCODE -ne 0) {
        throw "语法检查失败: $($t.File) ($label)"
    }
}

Write-Host "`n全部宿主机测试与语法检查通过" -ForegroundColor Green
exit 0
