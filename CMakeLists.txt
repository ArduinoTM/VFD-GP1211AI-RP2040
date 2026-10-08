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
# 诊断输出开关（Debug / Release 条件编译）
#   auto（默认）：CMAKE_BUILD_TYPE=Debug ⇒ 开；其余（Release 等）⇒ 关
#   on / off    ：显式指定
# 打开时逐秒打印 rx/cmd/data/gdram_crc/pinmon/slave-dbg/pc_hist/waits 与判读结论，并采样
# 引脚跳变 / PIO PC 直方图；关闭时只留启动横幅与"出问题才打印"的告警（fatal/初始化失败/接线）。
# 例：cmake -S . -B build -DPICO_SDK_PATH=... -DVFD_DEBUG_DIAG=on
# ---------------------------------------------------------------------------
set(VFD_DEBUG_DIAG "auto" CACHE STRING "诊断输出：auto|on|off（auto = Debug 构建开，其余关）")
set_property(CACHE VFD_DEBUG_DIAG PROPERTY STRINGS auto on off)
if(VFD_DEBUG_DIAG STREQUAL "auto")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_vfd_debug_diag 1)
    else()
        set(_vfd_debug_diag 0)
    endif()
elseif(VFD_DEBUG_DIAG)
    set(_vfd_debug_diag 1)
else()
    set(_vfd_debug_diag 0)
endif()
target_compile_definitions(vfd_gp1211ai_demo PRIVATE VFD_DEBUG_DIAG=${_vfd_debug_diag})
message(STATUS "诊断输出: VFD_DEBUG_DIAG=${VFD_DEBUG_DIAG} ⇒ 宏 VFD_DEBUG_DIAG=${_vfd_debug_diag}")

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
else()
    pico_enable_stdio_uart(vfd_gp1211ai_demo 0)
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
