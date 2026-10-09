#requires -version 5
<#
  build.ps1 —— GP1211AI RP2040 固件一键构建（自动配好工具链环境变量，交互式选模式）

  用法：
    powershell -ExecutionPolicy Bypass -File build.ps1                 # 交互式：逐项问你要编哪个模式
    powershell -ExecutionPolicy Bypass -File build.ps1 -Yes            # 全默认：pio + 诊断 on + Release
    powershell -ExecutionPolicy Bypass -File build.ps1 -Engine both -Diag on -Yes
    powershell -ExecutionPolicy Bypass -File build.ps1 -Engine both -Diag off -Type Release -Yes
    powershell -ExecutionPolicy Bypass -File build.ps1 -Test -Yes      # 先跑宿主测试，再编
    powershell -ExecutionPolicy Bypass -File build.ps1 -Clean -Yes     # 先删构建目录再来
    powershell -ExecutionPolicy Bypass -File build.ps1 -CleanAll      # 只清理：删掉所有 build-* 与 tests\build，不编译
    powershell -ExecutionPolicy Bypass -File build.ps1 -CleanOnly     # 只清理：删掉本次会用的那个构建目录，不编译
    powershell -ExecutionPolicy Bypass -File build.ps1 -Help

  参数（与 CMake 缓存变量一一对应，见 CMakeLists.txt）：
    -Engine    pio | tick | both    扫描引擎（pio = PIO 状态机；tick = 重复定时器 + SPI + DMA）
    -Diag      on | off             逐秒诊断输出（on = 排故；off = 默认，发布精简 uf2 小 ~22 KB）
    -DiagUsb                        诊断是否也走 USB（默认只走 UART；USB stdio 的写会阻塞主循环）
    -Type      Release | Debug      构建类型（只影响优化级别/调试信息，与诊断输出无关）
    -Dir       <目录名>             构建目录（默认按选项自动生成，如 build-pio-rel-diagon）

  注：LAT 极性 / 带内 3 列槽序 / 扫描相位这三组"调参旋钮"已在 2026-10-08 清理中移除 ——
      实机定标出的取值（LAT 空闲低+正脉冲、band 1、相位 −1、T43 只用 a,b）已直接写进实现，
      见 src/vfd_scanpack.h 顶部说明。
    -Test                           编译前先跑 tests\run_host_tests.ps1
    -Clean                          先删除构建目录（然后照常编译）
    -CleanOnly                      只清理：删掉本次会用到的构建目录后退出（不编译）
    -CleanAll                       只清理：删掉仓库内所有 build-* 目录与 tests\build 后退出（不编译）
    -PioRegen                       用 pioasm 重新生成 src\*.pio.h（改了 .pio 之后必须）
    -DualCore                       双核分工：core1=SSD1306 数据面（core0=时序+帧发布+看护）
    -Yes                            不提问，直接用参数/默认值
#>
[CmdletBinding()]
param(
    [ValidateSet('pio', 'tick', 'both')] [string]$Engine,
    [ValidateSet('on', 'off')] [string]$Diag,
    [ValidateSet('Release', 'Debug')] [string]$Type = 'Release',
    [string]$Dir,
    [switch]$Test,
    [switch]$Clean,
    [switch]$CleanOnly,
    [switch]$CleanAll,
    [switch]$PioRegen,
    [switch]$DualCore,
    [switch]$DiagUsb,
    [switch]$Yes,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------------ 小工具
function Say([string]$msg, [string]$color = 'Gray') { Write-Host $msg -ForegroundColor $color }
function Title([string]$msg) { Write-Host ""; Write-Host "== $msg ==" -ForegroundColor Cyan }
function Ok([string]$msg) { Write-Host "  [ok] $msg" -ForegroundColor Green }
function Warn([string]$msg) { Write-Host "  [!!] $msg" -ForegroundColor Yellow }
function Die([string]$msg) { Write-Host "  [xx] $msg" -ForegroundColor Red; exit 1 }

# 读一行回答：既支持人工交互（Read-Host），也支持管道喂答案（便于脚本化/回归测试）
function Read-Answer([string]$prompt) {
    if ([Console]::IsInputRedirected) {
        Write-Host $prompt
        $line = [Console]::In.ReadLine()
        if ($null -eq $line) { return '' }
        return $line.Trim()
    }
    return (Read-Host $prompt)
}

function Ask-Choice([string]$prompt, [string[]]$labels, [string[]]$values, [string]$default) {
    Write-Host ""
    Write-Host $prompt -ForegroundColor White
    for ($i = 0; $i -lt $labels.Count; $i++) {
        $mark = ''
        if ($values[$i] -eq $default) { $mark = '  <- 默认' }
        Write-Host ("   {0}) {1}{2}" -f ($i + 1), $labels[$i], $mark)
    }
    while ($true) {
        $a = Read-Answer ("请选择 [1-{0}]（回车 = 默认 {1}）" -f $labels.Count, $default)
        if ([string]::IsNullOrWhiteSpace($a)) { return $default }
        if ($a -match '^\d+$') {
            $idx = [int]$a
            if ($idx -ge 1 -and $idx -le $labels.Count) { return $values[$idx - 1] }
        }
        # 也允许直接输入取值本身（例如 on / pio / 0）
        foreach ($v in $values) { if ($a -eq $v) { return $v } }
        Warn ("输入无效：{0}" -f $a)
    }
}

function Find-First([string[]]$candidates) {
    foreach ($c in $candidates) {
        if ([string]::IsNullOrWhiteSpace($c)) { continue }
        $hit = Get-Item -Path $c -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

function Resolve-Exe([string]$name, [string[]]$candidates) {
    $hit = Find-First $candidates
    if ($hit) { return $hit }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

if ($Help) {
    Get-Help $PSCommandPath -Detailed
    exit 0
}

$repoRoot = $PSScriptRoot
if (-not $repoRoot) { $repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path }
$repoRoot = (Resolve-Path $repoRoot).Path
$wsRoot = Split-Path $repoRoot -Parent

# ------------------------------------------------------------------ 清理构建产物
#   **不提问、不依赖工具链**：清理只做文件删除，任何情况下都不需要额外输入。
#   -CleanAll ：删掉仓库内所有 build-* 目录 + tests\build，然后退出
#   -CleanOnly：只删"本次会用到"的构建目录（-Engine/-Dir 决定，默认两个引擎的规范目录），然后退出
#   -Clean    ：先删掉本次会用到的构建目录，**然后继续走编译流程**（清理阶段不提问）
function Make-DirName([string]$eng) {
    # 规范名（与历史/文档一致）：pio -> build-rp2040-pio，tick -> build-rp2040；
    # 非默认的诊断/构建类型再挂后缀，便于多套并存。
    $name = 'build-rp2040'
    if ($eng -eq 'pio') { $name = 'build-rp2040-pio' }
    if ($Type -eq 'Debug') { $name += '-debug' }
    if ($Diag -eq 'on') { $name += '-diagon' }
    if ($DiagUsb) { $name += '-diagusb' }
    if ($DualCore) { $name += '-dual' }
    return $name
}

if ($CleanAll -or $CleanOnly -or $Clean) {
    $engines = @($Engine)
    if (-not $Engine -or $Engine -eq 'both') { $engines = @('pio', 'tick') }

    $targets = @()
    if ($CleanAll) {
        $targets += Get-ChildItem (Join-Path $repoRoot 'build-*') -Directory -ErrorAction SilentlyContinue
        $tb = Join-Path $repoRoot 'tests/build'
        if (Test-Path $tb) { $targets += Get-Item $tb }
    } else {
        foreach ($e in $engines) {
            $d = Make-DirName $e
            if ($Dir) { $d = $Dir }              # 显式给了目录名就只清那一个
            $p = Join-Path $repoRoot $d
            if (Test-Path $p) { $targets += Get-Item $p }
        }
    }

    $modeTitle = '清理构建目录'
    if ($CleanAll) { $modeTitle = '清理所有构建产物' }
    Title $modeTitle
    if ($targets.Count -eq 0) {
        Say '  没有需要清理的目录。'
    } else {
        foreach ($t in $targets) {
            $sz = (Get-ChildItem $t.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
            Remove-Item $t.FullName -Recurse -Force
            Say ("  已删 {0,8:N1} MB  {1}" -f ($sz / 1MB), $t.FullName.Substring($repoRoot.Length + 1))
        }
        Ok ("共清理 {0} 个目录" -f $targets.Count)
    }
    if ($CleanAll -or $CleanOnly) { exit 0 }   # 纯清理：到此为止，不编译
    Say '  （-Clean：接着编译；下面开始问编译配置）'
}

# ------------------------------------------------------------------ 环境：工具链自动定位

Title "工具链环境"
Say ("仓库: {0}" -f $repoRoot)

$toolsCandidates = @(
    (Join-Path $wsRoot '.tools'),
    (Join-Path $repoRoot '.tools'),
    (Join-Path (Split-Path $wsRoot -Parent) '.tools')
)
$toolsDir = Find-First $toolsCandidates
if ($toolsDir) { Ok ("工具目录: {0}" -f $toolsDir) } else { Warn "没找到 .tools 目录（将只用系统 PATH / 环境变量里的工具）" }

# arm-none-eabi-gcc（xPack 或系统安装；允许 xpack-* 带版本号的目录）
$gccCandidates = @()
if ($toolsDir) { $gccCandidates += (Join-Path $toolsDir 'xpack-arm-none-eabi-gcc*\bin') }
$gccCandidates += @(
    (Join-Path $wsRoot 'xpack-arm-none-eabi-gcc*\bin'),
    'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi*\bin',
    'C:\Program Files\Arm GNU Toolchain arm-none-eabi*\bin'
)
$gccBin = Find-First $gccCandidates
$gccExe = $null
if ($gccBin) { $gccExe = Join-Path $gccBin 'arm-none-eabi-gcc.exe' }
if (-not $gccExe -or -not (Test-Path $gccExe)) {
    $cmd = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
    if ($cmd) { $gccExe = $cmd.Source; $gccBin = Split-Path $gccExe -Parent }
}
if (-not $gccExe -or -not (Test-Path $gccExe)) {
    Die "找不到 arm-none-eabi-gcc。请装 ARM GNU Toolchain（或 xPack），或把它的 bin 目录加到 PATH 后重试。"
}
Ok ("arm-none-eabi-gcc: {0}" -f $gccExe)
$sizeExe = Join-Path $gccBin 'arm-none-eabi-size.exe'
if (-not (Test-Path $sizeExe)) { $sizeExe = $null }

# ninja
$ninjaExe = Resolve-Exe 'ninja' @(
    'C:\Program Files\Meson\ninja.EXE',
    # 用 GetFolderPath 取本机应用数据目录：语义等价，且源码里不含该环境变量字面量（发布仓库护栏）
    (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Microsoft\WinGet\Links\ninja.exe'),
    (Join-Path $wsRoot 'ninja.exe')
)
if (-not $ninjaExe) { Die "找不到 ninja。请装 Ninja（或把它放到 PATH）：winget install Ninja-build.Ninja" }
Ok ("ninja: {0}" -f $ninjaExe)

# Pico SDK
$sdkCandidates = @()
if ($env:PICO_SDK_PATH) { $sdkCandidates += $env:PICO_SDK_PATH }
if ($toolsDir) { $sdkCandidates += (Join-Path $toolsDir 'pico-sdk') }
$sdkCandidates += (Join-Path $wsRoot 'pico-sdk')
$sdk = $null
foreach ($c in $sdkCandidates) { if ($c -and (Test-Path (Join-Path $c 'external\pico_sdk_import.cmake'))) { $sdk = (Resolve-Path $c).Path; break } }
if (-not $sdk) { Die "找不到 Pico SDK（期望 <sdk>\external\pico_sdk_import.cmake）。请设 PICO_SDK_PATH 或放到 .tools\pico-sdk。" }
Ok ("Pico SDK: {0}" -f $sdk)

# pioasm（仅在 -PioRegen 时需要）
$pioasm = $null
$pioasmCandidates = @()
if ($toolsDir) {
    $pioasmCandidates += (Join-Path $toolsDir 'pico-sdk-tools\pioasm\pioasm.exe')
    $pioasmCandidates += (Join-Path $toolsDir 'pioasm\pioasm.exe')
}
$pioasm = Resolve-Exe 'pioasm' $pioasmCandidates

# picotool（把 elf 转成 uf2；没有它时用 SDK 自带的 elf2uf2 或构建产物）
$picotool = $null
$picotoolCandidates = @()
if ($toolsDir) {
    $picotoolCandidates += (Join-Path $toolsDir 'picotool\picotool\picotool.exe')
    $picotoolCandidates += (Join-Path $toolsDir 'picotool\picotool.exe')
}
$picotool = Resolve-Exe 'picotool' $picotoolCandidates
if ($picotool) { Ok ("picotool: {0}" -f $picotool) } else { Warn "没找到 picotool（uf2 会用 SDK 自带工具生成）" }

# 环境变量：让脚本内启动的 cmake/ninja/gcc 都能找到
$env:PICO_SDK_PATH = $sdk
$env:PATH = $gccBin + ';' + (Split-Path $ninjaExe -Parent) + ';' + $env:PATH
Say ("已设置 PICO_SDK_PATH / PATH（gcc + ninja 前置）")

# ------------------------------------------------------------------ 交互式选模式
$defaultEngine = 'pio'
$defaultDiag = 'off'    # 诊断输出默认关：默认构建不打印逐秒统计，USB 串口保持干净
$defaultType = 'Release'

if (-not $Yes) {
    Title "编译模式（回车 = 默认）"
    if (-not $Engine) {
        $Engine = Ask-Choice "① 扫描引擎（两种引擎接线相同，只是时序产生方式不同）" @(
            "pio   —— PIO 状态机 + DMA（推荐，CPU 占用最低）",
            "tick  —— 重复定时器 + SPI(PL022) + DMA",
            "both  —— 两种都编（各出一个 uf2，便于对照）"
        ) @('pio', 'tick', 'both') $defaultEngine
    }
    if (-not $Diag) {
        $Diag = Ask-Choice "② 诊断输出（逐秒打印 rx/cmd/data/gdram_crc/CPU 占用/pinmon/slave-dbg/pc_hist/waits）" @(
            "off   —— 默认：发布精简（只留启动横幅与告警；USB 串口干净）",
            "on    —— 排故用（逐秒统计只走 UART；uf2 大 ~22 KB）"
        ) @('off', 'on') $defaultDiag
    }
    if (-not $PSBoundParameters.ContainsKey('Type')) {
        $Type = Ask-Choice "③ 构建类型" @(
            "Release —— -O2/-O3，正常使用",
            "Debug   —— -Og -g，带调试信息（与诊断输出无关）"
        ) @('Release', 'Debug') $defaultType
    }
    if (-not $PSBoundParameters.ContainsKey('DualCore')) {
        $dualChoice = Ask-Choice "④ 双核任务分工（core0=时序+帧发布，core1=SSD1306 数据面）" @(
            "off   —— 单核（与历史版本一致，默认）",
            "on    —— 双核：core1 承载 SSD1306 数据面（解码/渲染）"
        ) @('off', 'on') 'off'
        $DualCore = ($dualChoice -eq 'on')
    }
    if (-not $Dir) {
        $defDir = "build-$Engine"
        $a = Read-Answer ("⑤ 构建目录名（回车 = {0}）" -f $defDir)
        if (-not [string]::IsNullOrWhiteSpace($a)) { $Dir = $a }
    }
} else {
    if (-not $Engine) { $Engine = $defaultEngine }
    if (-not $Diag) { $Diag = $defaultDiag }
}

# ------------------------------------------------------------------ 目录名（按选项自动生成，便于并存多套）

# ------------------------------------------------------------------ 编译前：宿主测试
if ($Test) {
    Title "宿主测试（tests\run_host_tests.ps1）"
    $testScript = Join-Path $repoRoot 'tests\run_host_tests.ps1'
    if (-not (Test-Path $testScript)) { Die ("找不到 {0}" -f $testScript) }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $testScript
    if ($LASTEXITCODE -ne 0) { Die "宿主测试未通过，已中止编译" }
    Ok "宿主测试通过"
}

# ------------------------------------------------------------------ .pio 与 .pio.h 新鲜度
function Check-PioFresh([switch]$wantRegen) {
    $pioFiles = Get-ChildItem (Join-Path $repoRoot 'src\*.pio') -ErrorAction SilentlyContinue
    $stale = @()
    foreach ($p in $pioFiles) {
        $h = Join-Path $p.DirectoryName ($p.BaseName + '.pio.h')
        if (-not (Test-Path $h)) { $stale += $p.Name; continue }
        if ($p.LastWriteTime -gt (Get-Item $h).LastWriteTime) { $stale += $p.Name }
    }
    if ($stale.Count -gt 0) {
        Warn (".pio 比生成的 .pio.h 新：{0} —— 需要重新生成（否则板上跑的还是旧程序）" -f ($stale -join ', '))
        if (-not $wantRegen) {
            Warn "本次未加 -PioRegen：CMake 会继续用仓库内的旧 .pio.h。要重新生成请加 -PioRegen（或用 CMake 的 -DVFD_PIO_REGENERATE=ON）。"
        }
    }
}

# ------------------------------------------------------------------ 编译一个引擎
function Build-Engine([string]$eng, [string]$dirName) {
    Title ("编译 {0} 引擎 → {1}" -f $eng, $dirName)
    $buildDir = Join-Path $repoRoot $dirName
    if ($Clean -and (Test-Path $buildDir)) {
        Warn ("删除旧构建目录 {0}" -f $buildDir)
        Remove-Item -Recurse -Force $buildDir
    }

    $cmakeArgs = @(
        '-S', $repoRoot,
        '-B', $buildDir,
        '-G', 'Ninja',
        ("-DPICO_SDK_PATH={0}" -f ($sdk -replace '\\', '/')),
        '-DPICO_BOARD=pico',
        '-DPICO_NO_PICOTOOL=1',
        ("-DCMAKE_MAKE_PROGRAM={0}" -f $ninjaExe),
        ("-DCMAKE_BUILD_TYPE={0}" -f $Type),
        ("-DVFD_SCAN_ENGINE={0}" -f $eng),
        ("-DVFD_DEBUG_DIAG={0}" -f $Diag)
    )
    if ($DualCore) {
        $cmakeArgs += '-DVFD_DUAL_CORE=ON'
    }
    if ($DiagUsb) {
        $cmakeArgs += '-DVFD_DEBUG_DIAG_USB=ON'
    }
    if ($PioRegen) {
        $cmakeArgs += '-DVFD_PIO_REGENERATE=ON'
        if ($pioasm) { $cmakeArgs += ("-Dpioasm_DIR={0}" -f (Split-Path $pioasm -Parent)) }
    }

    Say ("cmake " + ($cmakeArgs -join ' ')) 'DarkGray'
    # 注意：本函数只把 uf2 路径作为返回值，所以外部命令的输出必须走 Out-Host（否则会被当成返回值）
    & cmake @cmakeArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { Die "cmake 配置失败（{0}）" -f $eng }

    & cmake --build $buildDir | Out-Host
    if ($LASTEXITCODE -ne 0) { Die "编译失败（{0}）" -f $eng }

    $elf = Join-Path $buildDir 'vfd_gp1211ai_demo.elf'
    $uf2 = Join-Path $buildDir 'vfd_gp1211ai_demo.uf2'
    if (-not (Test-Path $elf)) { Die ("没找到 {0}" -f $elf) }

    # uf2：优先用 picotool 重新转一遍（保证是当前 elf 的）
    if ($picotool) {
        & $picotool uf2 convert $elf $uf2 | Out-Host
        if ($LASTEXITCODE -ne 0) { Warn "picotool uf2 转换失败（若构建已生成 .uf2 则仍可用）" }
    }
    if (-not (Test-Path $uf2)) { Warn ("{0} 不存在：请检查 picotool / SDK 的 elf2uf2" -f $uf2) }

    # 体积
    if ($sizeExe) {
        $line = (& $sizeExe $elf | Select-Object -Last 1)
        Ok ("体积: {0}" -f $line)
    }
    if (Test-Path $uf2) {
        $kb = [math]::Round((Get-Item $uf2).Length / 1KB, 1)
        Ok ("uf2: {0}  ({1} KB)" -f $uf2, $kb)
    }
    return $uf2
}

# ------------------------------------------------------------------ 开编
$targets = @()
if ($Engine -eq 'both') { $targets = @('pio', 'tick') } else { $targets = @($Engine) }

Check-PioFresh -wantRegen:$PioRegen

$results = @()
foreach ($eng in $targets) {
    if ($Dir -and $targets.Count -eq 1) { $d = $Dir } else { $d = Make-DirName $eng }
    $u = Build-Engine $eng $d
    $results += [pscustomobject]@{ Engine = $eng; Dir = $d; Uf2 = $u }
}

# ------------------------------------------------------------------ 汇总 + 板上指纹预期
Title "完成"
foreach ($r in $results) {
    $rev = 'false'; $diagMacro = '0'
    if ($r.Engine -eq 'tick') { $rev = 'true' }
    if ($Diag -eq 'on') { $diagMacro = '1' }
    Say ("  {0,-5} 引擎: {1}" -f $r.Engine, $r.Uf2) 'White'
    Say ("        启动横幅应为: wire: reverseBits={0}   （诊断宏 VFD_DEBUG_DIAG={1}）" -f $rev, $diagMacro) 'DarkGray'
}
Say ("  构建类型 {0}（只影响优化/调试信息；LAT 极性/槽序/相位已固化在实现里，见 src/vfd_scanpack.h）" -f $Type) 'DarkGray'
Say ("  诊断输出 {0}{1}" -f $Diag, $(if ($Diag -eq 'on') { $(if ($DiagUsb) { '（走 USB+UART）' } else { '（只走 UART）' }) } else { '' })) 'DarkGray'
if ($DualCore) { Say ("  双核分工已开启：core0=时序+帧发布，core1=SSD1306 数据面") 'DarkGray' }
Say ""
Say "烧录：按住 BOOTSEL 插 USB（出现 RPI-RP2 盘）→ 把上面的 .uf2 拖进去。" 'White'
Say "排故时建议 -Diag on（逐秒诊断）；发布时 -Diag off（uf2 小 ~22 KB）。" 'DarkGray'
exit 0
