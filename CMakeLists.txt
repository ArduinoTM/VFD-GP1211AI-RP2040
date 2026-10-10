cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# Pico SDK 路径：优先使用 -DPICO_SDK_PATH=...，其次环境变量 PICO_SDK_PATH
# ---------------------------------------------------------------------------
if (NOT PICO_SDK_PATH)
    if (DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    endif()
endif()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR
        "未找到 Pico SDK。请设置环境变量 PICO_SDK_PATH，或用 -DPICO_SDK_PATH=<path> 指定。\n"
        "  例：cmake -S . -B build -DPICO_SDK_PATH=C:/pico/pico-sdk -DPICO_BOARD=pico")
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" ABSOLUTE)
message(STATUS "Pico SDK: ${PICO_SDK_PATH}")

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(vfd_gp1211ai_rp2040 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 扫描引擎：tick（默认，定时器 + SPI + DMA）或 pio（PIO 状态机 + DMA）
# 两种引擎接线完全相同，只是时序产生方式不同。
# ---------------------------------------------------------------------------
set(VFD_SCAN_ENGINE "tick" CACHE STRING "扫描引擎: tick = 定时器+SPI+DMA, pio = PIO 状态机+DMA")
set_property(CACHE VFD_SCAN_ENGINE PROPERTY STRINGS tick pio)

# PIO 程序头（vfd_scan.pio.h / ssd1306_spi_slave.pio.h）默认使用仓库内已生成的版本，
# 不依赖 pioasm；改了 .pio 之后用 -DVFD_PIO_REGENERATE=ON 重新生成
# （需要 pioasm；版本要与 SDK 匹配时可用 pico-sdk-tools 里的预编译 pioasm +
#   -Dpioasm_DIR=<...>/pico-sdk-tools/pioasm）。
option(VFD_PIO_REGENERATE "用 pioasm 重新生成 src/*.pio.h" OFF)

add_executable(vfd_gp1211ai_demo
    src/main.cpp
    src/demo_screens.cpp
    src/vfd_gp1211ai.cpp
    src/vfd_scanpack.cpp
    src/vfd_platform_rp2040.cpp
    src/ssd1306_emulator.cpp
    src/ssd1306_slave_rp2040.cpp
    src/compat/Print.cpp
    lib/Adafruit_GFX/Adafruit_GFX.cpp
)

if (VFD_SCAN_ENGINE STREQUAL "pio")
    target_sources(vfd_gp1211ai_demo PRIVATE src/vfd_platform_rp2040_pio.cpp)
    if (VFD_PIO_REGENERATE)
        pico_generate_pio_header(vfd_gp1211ai_demo ${CMAKE_CURRENT_LIST_DIR}/src/vfd_scan.pio
            OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
        message(STATUS "   PIO 头: 由 pioasm 重新生成到 src/vfd_scan.pio.h")
    else()
        message(STATUS "   PIO 头: 使用仓库内 src/vfd_scan.pio.h（重新生成请加 -DVFD_PIO_REGENERATE=ON）")
    endif()
    target_compile_definitions(vfd_gp1211ai_demo PRIVATE VFD_SCAN_ENGINE_PIO=1)
    message(STATUS "扫描引擎: pio（PIO 状态机 + DMA）")
elseif (VFD_SCAN_ENGINE STREQUAL "tick")
    target_sources(vfd_gp1211ai_demo PRIVATE src/vfd_platform_rp2040_tick.cpp)
    target_compile_definitions(vfd_gp1211ai_demo PRIVATE VFD_SCAN_ENGINE_PIO=0)
    message(STATUS "扫描引擎: tick（repeating_timer + SPI + DMA）")
else()
    message(FATAL_ERROR "VFD_SCAN_ENGINE 只能是 tick 或 pio（当前: ${VFD_SCAN_ENGINE}）")
endif()

# SSD1306 行为模拟（4 线 SPI 从机）也要用 PIO，因此无条件链接
if (VFD_PIO_REGENERATE)
    pico_generate_pio_header(vfd_gp1211ai_demo ${CMAKE_CURRENT_LIST_DIR}/src/ssd1306_spi_slave.pio
        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
    message(STATUS "   PIO 头: 由 pioasm 重新生成到 src/ssd1306_spi_slave.pio.h")
endif()
target_link_libraries(vfd_gp1211ai_demo PRIVATE hardware_pio)
message(STATUS "SSD1306 模拟: 4 线 SPI 从机（SCLK/MOSI/DC/CS/RESET，默认 GP11-GP15）")

target_include_directories(vfd_gp1211ai_demo PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/compat
    ${CMAKE_CURRENT_LIST_DIR}/lib/Adafruit_GFX
)

# ---------------------------------------------------------------------------
# 诊断输出开关。⚠️「诊断(diag)」和构建类型「Debug」是两回事：
#   构建类型只决定优化级别与调试信息（-O2 / -Og -g），**不会**自动打开诊断输出。
#   on        ：逐秒打印 rx/cmd/data/gdram_crc/CPU 占用/pinmon/slave-dbg/pc_hist/waits 与判读结论；
#   off（默认）：只留启动横幅与"出问题才打印"的告警（fatal / 初始化失败 / 接线未通）。
# 例：cmake -S . -B build -DPICO_SDK_PATH=... -DVFD_DEBUG_DIAG=on
# ---------------------------------------------------------------------------
set(VFD_DEBUG_DIAG "off" CACHE STRING "诊断输出：on|off（默认 off）")
set_property(CACHE VFD_DEBUG_DIAG PROPERTY STRINGS on off)
string(TOLOWER "${VFD_DEBUG_DIAG}" _vfd_diag_lc) # -D 传 ON/OFF 也认（CMake 的 STREQUAL 是大小写敏感的）
if(_vfd_diag_lc STREQUAL "on")
    set(_vfd_debug_diag 1)
else()
    set(_vfd_debug_diag 0)
endif()
target_compile_definitions(vfd_gp1211ai_demo PRIVATE VFD_DEBUG_DIAG=${_vfd_debug_diag})
message(STATUS "诊断输出: VFD_DEBUG_DIAG=${VFD_DEBUG_DIAG} ⇒ 宏 VFD_DEBUG_DIAG=${_vfd_debug_diag}")

# 诊断输出是否也走 USB。默认 off：诊断只从 UART 输出，USB 串口保持干净
# （USB stdio 的写会阻塞主循环，排故时不想因此扰动时序）。on 则诊断同时走 USB 与 UART。
set(VFD_DEBUG_DIAG_USB "off" CACHE STRING "诊断是否也走 USB：on|off（默认 off = 只走 UART）")
set_property(CACHE VFD_DEBUG_DIAG_USB PROPERTY STRINGS on off)
string(TOLOWER "${VFD_DEBUG_DIAG_USB}" _vfd_diag_usb_lc)
if(_vfd_diag_usb_lc STREQUAL "on")
    target_compile_definitions(vfd_gp1211ai_demo PRIVATE VFD_DEBUG_DIAG_USB=1)
    message(STATUS "诊断 USB 输出: 开（诊断同时走 USB 与 UART）")
else()
    message(STATUS "诊断 USB 输出: 关（诊断只走 UART）")
endif()

# ---------------------------------------------------------------------------
# 双核任务分工（core0 = 时序 + 帧发布 + 看护；core1 = SSD1306 数据面）。
# 默认关（单核，行为与历史版本一致）；打开后 core1 用 multicore_launch_core1 启动。
# 资源不冲突：扫描引擎（core0）用 pio0/SPI0 + DMA0/1，SSD1306 从机（core1）用 pio1 + DMA2。
# ---------------------------------------------------------------------------
option(VFD_DUAL_CORE "双核分工：core0=时序+帧发布，core1=SSD1306 数据面" OFF)
if (VFD_DUAL_CORE)
    target_compile_definitions(vfd_gp1211ai_demo PRIVATE VFD_DUAL_CORE=1)
    target_link_libraries(vfd_gp1211ai_demo PRIVATE pico_multicore)
    message(STATUS "双核分工: 开（core1 = SSD1306 数据面，pico_multicore）")
else()
    message(STATUS "双核分工: 关（单核，与历史版本一致）")
endif()

target_compile_options(vfd_gp1211ai_demo PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

target_link_libraries(vfd_gp1211ai_demo PRIVATE
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_timer
    hardware_clocks
)

# USB + UART 双份 stdio（printf 调试用）
option(VFD_STDIO_USB "启用 USB CDC stdio（需要 pico-sdk 的 tinyusb 子模块）" ON)
option(VFD_STDIO_UART "启用 UART stdio" ON)

if (VFD_STDIO_USB)
    pico_enable_stdio_usb(vfd_gp1211ai_demo 1)
else()
    pico_enable_stdio_usb(vfd_gp1211ai_demo 0)
    message(STATUS "USB stdio 已关闭（如未初始化 tinyusb 子模块，请再加 -DPICO_STDIO_USB_ENABLED=0）")
endif()

if (VFD_STDIO_UART)
    pico_enable_stdio_uart(vfd_gp1211ai_demo 1)
    # ⚠️ pico_enable_stdio_uart() 只会注入 LIB_PICO_STDIO_UART（库存在标志），**不会**注入
    #    PICO_STDIO_UART。诊断落点要用它判断"UART 是否可用"，所以在这里自己注入一个明确的宏。
    target_compile_definitions(vfd_gp1211ai_demo PRIVATE VFD_STDIO_UART_ENABLED=1)
else()
    pico_enable_stdio_uart(vfd_gp1211ai_demo 0)
    target_compile_definitions(vfd_gp1211ai_demo PRIVATE VFD_STDIO_UART_ENABLED=0)
endif()

pico_add_extra_outputs(vfd_gp1211ai_demo)

# ---------------------------------------------------------------------------
# 可选：覆盖默认接线 / 引擎参数，例如
#   cmake -S . -B build -DPICO_SDK_PATH=... -DVFD_SCAN_ENGINE=pio -DVFD_PIN_BK=15
# ---------------------------------------------------------------------------
set(VFD_PIN_OPTIONS
    VFD_PIN_CLKA VFD_PIN_SIA VFD_PIN_LAT VFD_PIN_CLKG
    VFD_PIN_SIG VFD_PIN_BK VFD_PIN_HV_EN VFD_PIN_FL_EN VFD_PIN_TEST_MODE
    VFD_DMA_TX_CH VFD_DMA_RX_CH
    VFD_PIO_INST VFD_PIO_SM VFD_DMA_CH VFD_PIO_CLK_HZ
    VFD_EMU_PIN_SCK VFD_EMU_PIN_MOSI VFD_EMU_PIN_DC VFD_EMU_PIN_CS VFD_EMU_PIN_RESET
    VFD_EMU_PIO_INST VFD_EMU_PIO_SM VFD_EMU_DMA_CH
)
foreach(opt IN LISTS VFD_PIN_OPTIONS)
    if (DEFINED ${opt})
        target_compile_definitions(vfd_gp1211ai_demo PRIVATE ${opt}=${${opt}})
        message(STATUS "引脚覆盖: ${opt}=${${opt}}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# 宿主测试接入 CTest：一条 `ctest` 跑完所有"不需要硬件"的回归。
# 真正的构建/运行逻辑仍在 tests/run_host_tests.ps1 里 —— CI 与本地共用同一个入口，
# 避免"两套测试脚本各跑各的"。
# 注：本工程配置需要 Pico SDK（固件目标），但测试本身不需要硬件。
# ---------------------------------------------------------------------------
enable_testing()
find_program(VFD_PWSH_EXE NAMES pwsh powershell)
if(VFD_PWSH_EXE)
    add_test(NAME host_tests
        COMMAND ${VFD_PWSH_EXE} -NoProfile -ExecutionPolicy Bypass -File
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_host_tests.ps1)
    set_tests_properties(host_tests PROPERTIES TIMEOUT 900)
else()
    message(STATUS "未找到 pwsh/powershell ⇒ 不注册 host_tests（可手动跑 tests/run_host_tests.ps1）")
endif()
