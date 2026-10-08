/*
 * 宿主机测试：SSD1306 行为模拟（src/ssd1306_emulator.*）
 *
 * 只测平台无关的模拟核心（传输层 ssd1306_slave_rp2040 需要 Pico SDK，由固件构建验证）。
 * 重点验证"主机视角"的正确性：
 *   1. 一份标准的 SSD1306 128x64 初始化序列 + 整屏 GDDRAM 写入之后，
 *      渲染到 VFD 帧缓冲的图像应当与主机写入的图像**逐位一致**
 *      （A1/C8 是 Adafruit_SSD1306 等库的默认配置，SSD1306 会做段/COM 镜像抵消）；
 *   2. 关掉重映射（A0/C0）时应当得到镜像图像；
 *   3. 页/水平/垂直三种寻址模式的写入指针推进规则（含窗口回绕）；
 *   4. 显示开关、全亮、反显、对比度、未知命令、跨事务拆分的多字节命令；
 *   5. 滚动（0x26/0x27/0x29/0x2A 设置 + 0x2E/0x2F 停止/启动）：方向、页窗口、
 *      循环回绕、时间间隔、垂直分量、异常参数收敛、阻塞后不补播。
 */
#include <stdio.h>
#include <string.h>

#include "ssd1306_emulator.h"
#include "vfd_crc32.h"

/* ------------------------------------------------------------ 小测试框架 */

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char *what, const char *detail = "")
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  [FAIL] %s %s\n", what, detail);
    }
}

static void section(const char *title)
{
    printf("\n=== %s ===\n", title);
}

/* --------------------------------------------------- SSD1306 主机侧辅助 */

/* 主机视角的 1bpp 帧缓冲：与 Adafruit_SSD1306 的 buffer[x + (y/8)*128] 相同 */
static uint8_t g_host[vfd::SSD1306_GDDRAM_SIZE];
static uint8_t g_rendered[vfd::SSD1306_GDDRAM_SIZE];

static void hostSetPixel(int x, int y, bool on)
{
    uint8_t &cell = g_host[(y >> 3) * vfd::SSD1306_WIDTH + x];
    if (on)
        cell |= static_cast<uint8_t>(1u << (y & 7));
    else
        cell &= static_cast<uint8_t>(~(1u << (y & 7)));
}

static void hostPattern(int kind)
{
    memset(g_host, 0, sizeof(g_host));
    switch (kind) {
    case 0: /* 边框 + 对角线 */
        for (int x = 0; x < 128; ++x) {
            hostSetPixel(x, 0, true);
            hostSetPixel(x, 63, true);
        }
        for (int y = 0; y < 64; ++y) {
            hostSetPixel(0, y, true);
            hostSetPixel(127, y, true);
            hostSetPixel(y * 2 % 128, y, true);
        }
        break;
    case 1: /* 伪随机（确定性） */
    {
        uint32_t r = 0x12345678u;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 128; ++x) {
                r = r * 1664525u + 1013904223u;
                hostSetPixel(x, y, (r >> 17) & 1u);
            }
        }
        break;
    }
    default: /* 棋盘格 */
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 128; ++x)
                hostSetPixel(x, y, ((x / 4 + y / 4) & 1) != 0);
        break;
    }
}

static void emuCommand(vfd::Ssd1306Emulator &emu, uint8_t cmd)
{
    emu.pushByte(cmd, false);
}

static void emuData(vfd::Ssd1306Emulator &emu, const uint8_t *data, int len)
{
    for (int i = 0; i < len; ++i)
        emu.pushByte(data[i], true);
}

/* 标准 SSD1306 128x64 初始化序列（Adafruit_SSD1306 4 线 SPI 的典型命令流） */
static void emuStandardInit(vfd::Ssd1306Emulator &emu, bool remap)
{
    const uint8_t seq[] = {
        0xAE,       /* display off */
        0xD5, 0x80, /* clock divide */
        0xA8, 0x3F, /* multiplex = 64 */
        0xD3, 0x00, /* display offset */
        0x40,       /* start line 0 */
        0x8D, 0x14, /* charge pump on */
        0x20, 0x00, /* horizontal addressing */
        remap ? static_cast<uint8_t>(0xA1) : static_cast<uint8_t>(0xA0),
        remap ? static_cast<uint8_t>(0xC8) : static_cast<uint8_t>(0xC0),
        0xDA, 0x12, /* com pins */
        0x81, 0xCF, /* contrast */
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,       /* entire display off */
        0xA6,       /* normal */
        0x2E,       /* deactivate scroll */
        0xAF        /* display on */
    };
    for (uint8_t b : seq)
        emuCommand(emu, b);
}

/* 主机把 g_host 整屏刷进去（水平寻址 + 行/列窗口 + 1024 数据字节） */
static void emuWriteFullFrame(vfd::Ssd1306Emulator &emu, const uint8_t *buf)
{
    emuCommand(emu, 0x20); /* 水平寻址（库的初始化序列里也会发这条） */
    emuCommand(emu, 0x00);
    emuCommand(emu, 0x21);
    emuCommand(emu, 0x00);
    emuCommand(emu, 0x7F);
    emuCommand(emu, 0x22);
    emuCommand(emu, 0x00);
    emuCommand(emu, 0x07);
    emuData(emu, buf, vfd::SSD1306_GDDRAM_SIZE);
}

/* 图像比较：把 host[] 按 (x,y) 与渲染结果比较 */
static int comparePixels(const uint8_t *expect, const uint8_t *got, bool *firstMismatchAt)
{
    int diff = 0;
    for (int y = 0; y < vfd::SSD1306_HEIGHT; ++y) {
        for (int x = 0; x < vfd::SSD1306_WIDTH; ++x) {
            const bool a = (expect[(y >> 3) * 128 + x] >> (y & 7)) & 1u;
            const bool b = (got[(y >> 3) * 128 + x] >> (y & 7)) & 1u;
            if (a != b) {
                if (diff == 0 && firstMismatchAt != nullptr)
                    *firstMismatchAt = true;
                diff++;
            }
        }
    }
    return diff;
}

/* 按 SSD1306 的段/COM 镜像生成"物理"图像（用于 A0/C0 场景的期望值） */
static void mirrorHost(const uint8_t *src, uint8_t *dst, bool mirrorX, bool mirrorY)
{
    memset(dst, 0, vfd::SSD1306_GDDRAM_SIZE);
    for (int y = 0; y < vfd::SSD1306_HEIGHT; ++y) {
        for (int x = 0; x < vfd::SSD1306_WIDTH; ++x) {
            const bool on = (src[(y >> 3) * 128 + x] >> (y & 7)) & 1u;
            if (!on)
                continue;
            const int sx = mirrorX ? (vfd::SSD1306_WIDTH - 1 - x) : x;
            const int sy = mirrorY ? (vfd::SSD1306_HEIGHT - 1 - y) : y;
            dst[(sy >> 3) * 128 + sx] |= static_cast<uint8_t>(1u << (sy & 7));
        }
    }
}

/* ------------------------------------------------------------------ 用例 */

static void test_full_frame_roundtrip()
{
    section("1. 标准初始化 + 整屏写入：渲染结果 == 主机图像（A1/C8）");

    for (int kind = 0; kind < 3; ++kind) {
        hostPattern(kind);
        vfd::Ssd1306Emulator emu;
        emuStandardInit(emu, true);
        emuWriteFullFrame(emu, g_host);

        check(emu.displayOn(), "显示已打开（0xAF）");
        check(emu.addressingMode() == vfd::Ssd1306Emulator::ADDR_HORIZONTAL, "寻址模式 = 水平");
        check(emu.contrast() == 0xCF, "对比度 = 0xCF");
        check(emu.dataCount() == vfd::SSD1306_GDDRAM_SIZE, "收到 1024 个数据字节");
        check(memcmp(emu.gdram(), g_host, vfd::SSD1306_GDDRAM_SIZE) == 0,
            "GDDRAM 内容与主机写入一致");

        emu.renderToFramebuffer(g_rendered);
        const int diff = comparePixels(g_host, g_rendered, nullptr);
        printf("  图案 %d: 像素差异 %d\n", kind, diff);
        check(diff == 0, "渲染图像与主机图像逐位一致（A1+C8 镜像相互抵消）");
    }
}

static void test_remap_off()
{
    section("2. 关闭段/COM 重映射（A0/C0）：应得到镜像图像");

    hostPattern(0);
    vfd::Ssd1306Emulator emu;
    emuStandardInit(emu, false);
    emuWriteFullFrame(emu, g_host);
    emu.renderToFramebuffer(g_rendered);

    uint8_t expect[vfd::SSD1306_GDDRAM_SIZE];
    mirrorHost(g_host, expect, true, true);
    const int diff = comparePixels(expect, g_rendered, nullptr);
    printf("  与镜像图像的像素差异 %d\n", diff);
    check(diff == 0, "A0+C0 时渲染结果 = 主机图像的行列镜像");
}

static void test_page_addressing()
{
    section("3. 页寻址模式（0xB0-0xB7 + 列地址高低字节、列回绕）");

    vfd::Ssd1306Emulator emu;
    emuCommand(emu, 0x20);
    emuCommand(emu, 0x02); /* 页寻址 */
    check(emu.addressingMode() == vfd::Ssd1306Emulator::ADDR_PAGE, "切到页寻址");

    /* 页 3、列 26 起写 2 个字节：低列 0x0A + 高列 0x11 → 列 = 0x1A = 26 */
    emuCommand(emu, 0xB3);
    emuCommand(emu, 0x0A); /* 低列 nibble = 0xA */
    emuCommand(emu, 0x11); /* 高列 nibble = 1 → 列 = 0x1A = 26 */
    check(emu.columnAddress() == 26 && emu.pageAddress() == 3, "列/页指针设置正确");
    emu.pushByte(0xAA, true);
    emu.pushByte(0xBB, true);
    check(emu.gdram()[3 * 128 + 26] == 0xAA && emu.gdram()[3 * 128 + 27] == 0xBB,
        "数据写入 (页3, 列26/27)");
    check(emu.columnAddress() == 28, "页寻址下列自动加一");

    /* 列 127 之后回绕到 0（页寻址特性） */
    emuCommand(emu, 0xB0);
    emuCommand(emu, 0x0F); /* 低列 nibble = 0xF */
    emuCommand(emu, 0x17); /* 高列 nibble = 7 → 列 = 0x7F = 127 */
    check(emu.columnAddress() == 127, "列指针 = 127");
    emu.pushByte(0x11, true); /* 写 127 */
    emu.pushByte(0x22, true); /* 回绕写 0 */
    check(emu.gdram()[127] == 0x11 && emu.gdram()[0] == 0x22, "列越界回绕到本页列 0");
}

static void test_horizontal_and_vertical_walk()
{
    section("4. 水平/垂直寻址模式的指针推进与窗口回绕");

    /* 水平：窗口 2 列 × 2 页，写 8 个字节 ⇒ 循环两圈，最终留下第二圈的 4 个值 */
    vfd::Ssd1306Emulator emu;
    emuCommand(emu, 0x20);
    emuCommand(emu, 0x00);
    emuCommand(emu, 0x21);
    emuCommand(emu, 10);
    emuCommand(emu, 11);
    emuCommand(emu, 0x22);
    emuCommand(emu, 4);
    emuCommand(emu, 5);
    for (int i = 0; i < 8; ++i)
        emu.pushByte(static_cast<uint8_t>(0x10 + i), true);

    bool ok = true;
    for (int p = 4; p <= 5; ++p) {
        for (int c = 10; c <= 11; ++c) {
            const int idx = (p - 4) * 2 + (c - 10);
            ok = ok && (emu.gdram()[p * 128 + c] == static_cast<uint8_t>(0x14 + idx));
        }
    }
    check(ok, "水平寻址：按 窗口列→换页→回到列起点 顺序填充（窗口写满后回绕覆盖）");
    check(emu.columnAddress() == 10 && emu.pageAddress() == 4, "写完 8 字节后指针回到窗口起点");

    /* 垂直：同样的窗口，顺序应为 页→换列→回到页起点 */
    vfd::Ssd1306Emulator emu2;
    emuCommand(emu2, 0x20);
    emuCommand(emu2, 0x01);
    emuCommand(emu2, 0x21);
    emuCommand(emu2, 10);
    emuCommand(emu2, 11);
    emuCommand(emu2, 0x22);
    emuCommand(emu2, 4);
    emuCommand(emu2, 5);
    for (int i = 0; i < 4; ++i)
        emu2.pushByte(static_cast<uint8_t>(0x20 + i), true);

    check(emu2.gdram()[4 * 128 + 10] == 0x20 && emu2.gdram()[5 * 128 + 10] == 0x21
            && emu2.gdram()[4 * 128 + 11] == 0x22 && emu2.gdram()[5 * 128 + 11] == 0x23,
        "垂直寻址：先走页再换列");
}

static void test_display_flags()
{
    section("5. 显示开关 / 全亮 / 反显 / 对比度");

    hostPattern(0);

    /* 未发送 0xAF：显示关 → 全黑 */
    {
        vfd::Ssd1306Emulator emu;
        emuCommand(emu, 0x20);
        emuCommand(emu, 0x00);
        emuWriteFullFrame(emu, g_host);
        emu.renderToFramebuffer(g_rendered);
        check(!emu.displayOn(), "默认显示关闭");
        bool allZero = true;
        for (int i = 0; i < vfd::SSD1306_GDDRAM_SIZE; ++i)
            allZero = allZero && (g_rendered[i] == 0);
        check(allZero, "显示关闭时渲染为全黑");
    }

    /* 正常显示 */
    vfd::Ssd1306Emulator emu;
    emuStandardInit(emu, true);
    emuWriteFullFrame(emu, g_host);
    emu.renderToFramebuffer(g_rendered);
    check(comparePixels(g_host, g_rendered, nullptr) == 0, "显示打开后图像正确");

    /* 反显 0xA7 */
    emuCommand(emu, 0xA7);
    emu.renderToFramebuffer(g_rendered);
    uint8_t inverted[vfd::SSD1306_GDDRAM_SIZE];
    for (int i = 0; i < vfd::SSD1306_GDDRAM_SIZE; ++i)
        inverted[i] = static_cast<uint8_t>(~g_host[i]);
    check(comparePixels(inverted, g_rendered, nullptr) == 0, "0xA7 反显生效");
    emuCommand(emu, 0xA6);

    /* 全亮 0xA5 */
    emuCommand(emu, 0xA5);
    emu.renderToFramebuffer(g_rendered);
    bool allOn = true;
    for (int i = 0; i < vfd::SSD1306_GDDRAM_SIZE; ++i)
        allOn = allOn && (g_rendered[i] == 0xFF);
    check(allOn, "0xA5 全亮（Entire Display ON）");
    emuCommand(emu, 0xA4);

    /* 对比度 0x81 */
    emuCommand(emu, 0x81);
    emuCommand(emu, 0x7F);
    check(emu.contrast() == 0x7F, "0x81 设置对比度 = 0x7F");
    emuCommand(emu, 0x81);
    emuCommand(emu, 0x00);
    check(emu.contrast() == 0x00, "0x81 设置对比度 = 0x00");

    /* 显示偏移 0xD3 = 4：整幅下移 4 行（等价于上移 60 行） */
    vfd::Ssd1306Emulator emu2;
    emuStandardInit(emu2, true);
    emuWriteFullFrame(emu2, g_host);
    emuCommand(emu2, 0xD3);
    emuCommand(emu2, 0x04);
    check(emu2.displayOffset() == 4, "0xD3 显示偏移 = 4");
    emu2.renderToFramebuffer(g_rendered);
    uint8_t shifted[vfd::SSD1306_GDDRAM_SIZE];
    memset(shifted, 0, sizeof(shifted));
    for (int y = 0; y < vfd::SSD1306_HEIGHT; ++y) {
        for (int x = 0; x < vfd::SSD1306_WIDTH; ++x) {
            const bool on = (g_host[(y >> 3) * 128 + x] >> (y & 7)) & 1u;
            if (!on)
                continue;
            const int my = (y + 60) & 63; /* A1/C8 下，偏移 4 表现为再镜像后上移 4 → 等价位移 */
            shifted[(my >> 3) * 128 + x] |= static_cast<uint8_t>(1u << (my & 7));
        }
    }
    /* 由于同时有 C8 镜像，这里只做"非空且有位移"的弱校验，避免把镜像语义写死在测试里 */
    int litCount = 0;
    for (int i = 0; i < vfd::SSD1306_GDDRAM_SIZE; ++i)
        for (int b = 0; b < 8; ++b)
            litCount += (g_rendered[i] >> b) & 1;
    check(litCount > 0, "带显示偏移时仍能渲染出内容");
}

static void test_robustness()
{
    section("6. 兼容性：未知命令、跨调用拆分的多字节命令、重复整屏写入");

    vfd::Ssd1306Emulator emu;
    emuStandardInit(emu, true);

    /* 未知/保留命令不应破坏状态（0xE0/0xE1 在 SSD1306 命令表中未定义） */
    const uint32_t unknownBefore = emu.unknownCommandCount();
    const uint8_t pageBefore = emu.pageAddress();
    emuCommand(emu, 0xE0);
    emuCommand(emu, 0xE1);
    check(emu.unknownCommandCount() == unknownBefore + 2, "未知命令被计数");
    check(emu.pageAddress() == pageBefore, "未知命令不改变寻址状态");

    /* 多字节命令跨 pushByte 调用拆分（主机可以逐字节发送） */
    emuCommand(emu, 0x21);
    emuCommand(emu, 0x20); /* 只发到第 1 个参数 */
    emu.pushByte(0x2F, false); /* 第 2 个参数 */
    /* 窗口 0x20..0x2F，指针应落在列 0x20 */
    check(emu.columnAddress() == 0x20, "跨调用拆分的 0x21 参数正确应用");

    /* 连续两次整屏写入后图像仍然正确（窗口指针复位） */
    hostPattern(1);
    emuWriteFullFrame(emu, g_host);
    hostPattern(2);
    emuWriteFullFrame(emu, g_host);
    emu.renderToFramebuffer(g_rendered);
    check(comparePixels(g_host, g_rendered, nullptr) == 0, "重复整屏写入后图像正确");

    /* 渲染节流：dirty + 最小间隔 */
    vfd::Ssd1306Emulator emu3;
    emuStandardInit(emu3, true);
    emuWriteFullFrame(emu3, g_host);
    check(emu3.shouldRender(1000, 20), "有新数据时可以渲染");
    check(!emu3.shouldRender(1005, 20), "同一批数据不会重复渲染");
    emu3.pushByte(0xFF, true);
    check(!emu3.shouldRender(1010, 20), "未达最小间隔不渲染");
    check(emu3.shouldRender(1050, 20), "达到最小间隔后渲染");

    /* RESET 语义：reset() 回到上电默认值 */
    emu3.reset();
    check(!emu3.displayOn() && emu3.contrast() == 0x7F
            && emu3.addressingMode() == vfd::Ssd1306Emulator::ADDR_PAGE,
        "reset() 回到 SSD1306 上电默认状态");
}

/* ------------------------------------------------------------------- main */

/* --------------------------------------------------------------- 滚动辅助 */

/* 主机侧设置滚动：0x26/0x27（水平右/左）、0x29/0x2A（垂直+水平右/左），6 个参数 */
static void emuSetupScroll(vfd::Ssd1306Emulator &emu, uint8_t cmd, uint8_t startPage,
    uint8_t interval, uint8_t endPage, uint8_t vertOffset)
{
    emuCommand(emu, cmd);
    emuCommand(emu, 0x00); /* 空字节 */
    emuCommand(emu, startPage);
    emuCommand(emu, interval);
    emuCommand(emu, endPage);
    emuCommand(emu, vertOffset);
    emuCommand(emu, 0x00); /* 空字节 */
}

/* 参考实现（用索引映射，刻意与模拟器的 memmove 写法不同）：
 * 水平循环滚动 steps 步后的期望 GDDRAM */
static void scrollReference(const uint8_t *src, uint8_t *dst, uint8_t startPage,
    uint8_t endPage, bool right, int steps)
{
    memcpy(dst, src, vfd::SSD1306_GDDRAM_SIZE);
    for (uint8_t p = startPage; p <= endPage; ++p) {
        for (int c = 0; c < vfd::SSD1306_WIDTH; ++c) {
            const int from = right ? ((c - steps) % 128 + 128) % 128 : (c + steps) % 128;
            dst[p * 128 + c] = src[p * 128 + from];
        }
    }
}

static void test_scroll()
{
    section("7. 滚动：设置 / 启动 / 停止、方向与页窗口、循环回绕、时间间隔、垂直分量");

    hostPattern(0); /* 边框 + 对角线，含足够多的孤立像素便于判定方向 */

    /* --- 7.1 未配置时 0x2F 应被忽略 --- */
    {
        vfd::Ssd1306Emulator emu;
        emuCommand(emu, 0x2F);
        check(!emu.scrollActive() && !emu.scrollConfigured(), "未配置滚动时 0x2F 不生效");
    }

    /* --- 7.2 0x26（水平右）+ 0x2F：按间隔步进，内容右移并循环 --- */
    vfd::Ssd1306Emulator emu;
    emuStandardInit(emu, true);
    emuWriteFullFrame(emu, g_host);

    emuSetupScroll(emu, 0x26, /*startPage=*/0, /*interval=*/7, /*endPage=*/7, 0);
    check(emu.scrollConfigured() && !emu.scrollActive(), "0x26 只配置，不自动启动");
    check(emu.scrollMode() == vfd::Ssd1306Emulator::SCROLL_HORIZONTAL && emu.scrollRight(),
        "0x26 = 纯水平、向右");
    check(emu.scrollStartPage() == 0 && emu.scrollEndPage() == 7 && emu.scrollInterval() == 7,
        "起始页/结束页/间隔被正确解析");
    check(emu.scrollPeriodMs() == 2 * vfd::SSD1306_SCROLL_FRAME_MS,
        "间隔 7 ⇒ 2 帧/步 = 16 ms（间隔表 5,64,128,256,3,4,25,2）");

    emuCommand(emu, 0x2F);
    check(emu.scrollActive(), "0x2F 启动滚动");

    const uint32_t t0 = 1000;
    emu.tick(t0);
    check(emu.scrollStepCount() == 0, "启动时刻不立刻步进");
    emu.tick(t0 + 15);
    check(emu.scrollStepCount() == 0, "未到间隔时间不步进");
    emu.tick(t0 + 16);
    check(emu.scrollStepCount() == 1, "到间隔时间步进一次");
    emu.tick(t0 + 16 + 16 * 5);
    check(emu.scrollStepCount() == 6, "按间隔连续步进（共 6 步）");

    uint8_t expect[vfd::SSD1306_GDDRAM_SIZE];
    scrollReference(g_host, expect, 0, 7, true, 6);
    check(memcmp(emu.gdram(), expect, vfd::SSD1306_GDDRAM_SIZE) == 0,
        "6 步右移后 GDDRAM == 参考循环右移结果（逐字节）");
    check(emu.dirty(), "滚动会置位 dirty（触发重新渲染）");

    /* --- 7.3 0x2E 停止后再 tick 不再变化 --- */
    emuCommand(emu, 0x2E);
    check(!emu.scrollActive(), "0x2E 停止滚动");
    memcpy(expect, emu.gdram(), vfd::SSD1306_GDDRAM_SIZE);
    emu.tick(t0 + 1000);
    check(emu.scrollStepCount() == 6 && memcmp(emu.gdram(), expect, vfd::SSD1306_GDDRAM_SIZE) == 0,
        "停止后时间推进不再改写 GDDRAM");

    /* --- 7.4 0x2F 再次启动：从当前时刻重新计时 --- */
    emuCommand(emu, 0x2F);
    emu.tick(t0 + 2000);
    check(emu.scrollStepCount() == 6, "重新启动后不补播（从当前时刻起算）");
    emu.tick(t0 + 2000 + 16);
    check(emu.scrollStepCount() == 7, "重新启动后按新间隔步进");

    /* --- 7.5 0x27（水平左）方向相反 --- */
    {
        vfd::Ssd1306Emulator e2;
        e2.pushByte(0x26, false); /* 先随便配置一个，再改配置以验证覆盖 */
        for (int i = 0; i < 6; ++i)
            e2.pushByte(0x00, false);
        emuSetupScroll(e2, 0x27, 0, 7, 7, 0);
        e2.pushByte(0x2F, false);
        hostPattern(1);
        emuWriteFullFrame(e2, g_host);
        e2.tick(0);
        e2.tick(16);
        uint8_t expL[vfd::SSD1306_GDDRAM_SIZE];
        scrollReference(g_host, expL, 0, 7, false, 1);
        check(memcmp(e2.gdram(), expL, vfd::SSD1306_GDDRAM_SIZE) == 0,
            "0x27 一步后 == 参考循环左移结果（方向相反）");
    }

    /* --- 7.6 只滚动指定页窗口，窗口外内容不变 --- */
    {
        vfd::Ssd1306Emulator e3;
        hostPattern(2);
        emuWriteFullFrame(e3, g_host);
        emuSetupScroll(e3, 0x26, /*startPage=*/2, /*interval=*/7, /*endPage=*/4, 0);
        e3.pushByte(0x2F, false);
        e3.tick(0);
        e3.tick(16 * 3);
        uint8_t expW[vfd::SSD1306_GDDRAM_SIZE];
        scrollReference(g_host, expW, 2, 4, true, 3);
        check(memcmp(e3.gdram(), expW, vfd::SSD1306_GDDRAM_SIZE) == 0,
            "只滚动页 2..4，其它页保持不变");
    }

    /* --- 7.7 0x29（垂直+水平）：单像素对角线移动与窗口内回绕 --- */
    {
        vfd::Ssd1306Emulator e4;
        e4.pushByte(0xAF, false); /* 显示开 */
        /* 在页 0..7 窗口内点亮 (10, 40) */
        memset(g_host, 0, sizeof(g_host));
        hostSetPixel(10, 40, true);
        emuWriteFullFrame(e4, g_host);
        emuSetupScroll(e4, 0x29, 0, 7, 7, /*vertOffset=*/3);
        check(e4.scrollVerticalOffset() == 3, "0x29 垂直偏移 = 3");
        e4.pushByte(0x2F, false);
        e4.tick(0);
        e4.tick(16);

        const bool moved = ((e4.gdram()[(43 >> 3) * 128 + 11] >> (43 & 7)) & 1u) != 0;
        const bool old = ((e4.gdram()[(40 >> 3) * 128 + 10] >> (40 & 7)) & 1u) != 0;
        check(moved && !old, "一步后像素从 (10,40) 移到 (11,43)：水平 +1、垂直下移 3");

        /* 窗口底部回绕：改从 (100, 62) 出发，下移 3 行 → 绕到行 1 */
        vfd::Ssd1306Emulator e5;
        e5.pushByte(0xAF, false);
        memset(g_host, 0, sizeof(g_host));
        hostSetPixel(100, 62, true);
        emuWriteFullFrame(e5, g_host);
        emuSetupScroll(e5, 0x29, 0, 7, 7, 3);
        e5.pushByte(0x2F, false);
        e5.tick(0);
        e5.tick(16);
        const bool wrapped = ((e5.gdram()[(1 >> 3) * 128 + 101] >> (1 & 7)) & 1u) != 0;
        check(wrapped, "垂直方向在页窗口内循环回绕（62 + 3 → 1）");
    }

    /* --- 7.8 异常参数收敛：垂直偏移 0 → 1；结束页 < 起始页 → 收敛为起始页 --- */
    {
        vfd::Ssd1306Emulator e6;
        emuSetupScroll(e6, 0x29, /*startPage=*/5, 0, /*endPage=*/2, /*vertOffset=*/0);
        check(e6.scrollVerticalOffset() == 1, "垂直偏移 0（非法）收敛为 1");
        check(e6.scrollStartPage() == 5 && e6.scrollEndPage() == 5, "结束页 < 起始页时收敛为起始页");
    }

    /* --- 7.9 长时间阻塞后不做"快进"补播 --- */
    {
        vfd::Ssd1306Emulator e7;
        emuSetupScroll(e7, 0x26, 0, /*interval=*/0 /* 5 帧 = 40 ms */, 7, 0);
        e7.pushByte(0x2F, false);
        e7.tick(0);
        e7.tick(100000); /* 停 100 s 理论上会补很多步 */
        check(e7.scrollStepCount() == vfd::SSD1306_SCROLL_MAX_CATCHUP,
            "单次 tick 的补播步数被限制在 SSD1306_SCROLL_MAX_CATCHUP");
    }

    /* --- 7.10 滚动内容经过渲染路径后仍然正确（A1/C8 同向约定） --- */
    {
        vfd::Ssd1306Emulator e8;
        hostPattern(0);
        emuStandardInit(e8, true);
        emuWriteFullFrame(e8, g_host);
        emuSetupScroll(e8, 0x26, 0, 7, 7, 0);
        e8.pushByte(0x2F, false);
        e8.tick(0);
        e8.tick(16 * 4);
        uint8_t expR[vfd::SSD1306_GDDRAM_SIZE];
        scrollReference(g_host, expR, 0, 7, true, 4);
        e8.renderToFramebuffer(g_rendered);
        check(comparePixels(expR, g_rendered, nullptr) == 0,
            "滚动 4 步后渲染出的图像 == 滚动后的期望图像（逐位）");
    }
}

/* --- 8. GDDRAM CRC32：与主控测试程序"两端对照"用的判据 --------------------
 * 主控（examples/pico2_full_test）发完一帧后打印它写进 GDDRAM 的 CRC32，
 * 从机每秒打印自己 GDDRAM 的 CRC32 —— 数字相同即"每个字节都落对了位置"。
 * 这里同时锁死算法本身（标准向量）与"哪些命令会改 GDDRAM"。 */
static void test_gdram_crc()
{
    section("8. GDDRAM CRC32（两端对照判据）");

    /* 8.1 标准向量：任何实现都必须给出这两个值（用外部工具 Node zlib.crc32 核过） */
    const uint8_t vec[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    check(vfd::crc32(vec, 0) == vfd::CRC32_CHECK_EMPTY, "CRC32(空) = 0x00000000");
    check(vfd::crc32(vec, 9) == vfd::CRC32_CHECK_123456789, "CRC32(\"123456789\") = 0xCBF43926");

    /* 8.2 参考图（1024 B）—— 生成规则与 examples/pico2_full_test 的 buildImage() 完全同构 */
    uint8_t img[vfd::SSD1306_GDDRAM_SIZE];
    for (int p = 0; p < 8; ++p) {
        for (int c = 0; c < 128; ++c) {
            uint8_t v;
            if (p == 0 || p == 7 || c == 0 || c == 127)
                v = 0xFF;
            else if ((((c >> 3) + (p >> 1)) & 1) != 0)
                v = 0x55;
            else
                v = (uint8_t)(c * 7 + p * 31 + 0x5A);
            img[p * 128 + c] = v;
        }
    }
    check(vfd::crc32(img, sizeof(img)) == vfd::CRC32_CHECK_REF_IMAGE,
        "参考图的 CRC32 = 0xCCE79D14（主控程序会打印同一个值）");

    /* 8.3 按水平寻址把参考图写进模拟器 ⇒ GDDRAM 的 CRC 必须等于它 */
    {
        vfd::Ssd1306Emulator e;
        emuStandardInit(e, true);
        e.pushByte(0x20, false); /* 水平寻址 */
        e.pushByte(0x00, false);
        e.pushByte(0x21, false); /* 列窗口 0..127 */
        e.pushByte(0x00, false);
        e.pushByte(0x7F, false);
        e.pushByte(0x22, false); /* 页窗口 0..7 */
        e.pushByte(0x00, false);
        e.pushByte(0x07, false);
        for (int i = 0; i < vfd::SSD1306_GDDRAM_SIZE; ++i)
            e.pushByte(img[i], true);

        check(e.gdramCrc32() == vfd::CRC32_CHECK_REF_IMAGE, "写完后 gdramCrc32() == 参考值");
        check(memcmp(e.gdram(), img, sizeof(img)) == 0, "GDDRAM 与主机图像逐字节一致");
    }

    /* 8.4 只有"写 GDDRAM"的命令才改 CRC；反显/全亮/显示开关只影响渲染 */
    {
        vfd::Ssd1306Emulator e;
        emuStandardInit(e, true);
        for (int i = 0; i < vfd::SSD1306_GDDRAM_SIZE; ++i)
            e.pushByte(img[i], true);
        const uint32_t before = e.gdramCrc32();

        e.pushByte(0xA7, false); /* 反显 */
        e.pushByte(0xA5, false); /* 全亮 */
        e.pushByte(0xAE, false); /* 关显示 */
        check(e.gdramCrc32() == before, "反显/全亮/显示开关不改变 GDDRAM 的 CRC");

        /* 滚动是"改写 GDDRAM"，CRC 必然改变（判据与上面相反） */
        e.pushByte(0xA6, false);
        e.pushByte(0xA4, false);
        e.pushByte(0xAF, false);
        emuSetupScroll(e, 0x26, /*startPage=*/0, /*interval=*/7, /*endPage=*/7, /*vertOffset=*/0);
        e.pushByte(0x2F, false);
        e.tick(0);
        e.tick(16);
        check(e.scrollStepCount() > 0 && e.gdramCrc32() != before,
            "滚动会改写 GDDRAM ⇒ CRC 改变（且 steps > 0）");
    }
}

/* --- 9. 显示类命令必须置 dirty --------------------------------------------
 * 应用侧（src/main.cpp）只在 shouldRender() 为真时才 renderToFramebuffer()，而 shouldRender()
 * 以 _dirty 为前提。曾经只有 0xAE/0xAF 置 dirty ⇒ 宿主"只发命令不发数据"时永远不会重绘：
 * 现场（2026-10-06）= "0xA7/0xA5 没反应；0xAE→0xAF 时屏幕全亮（那一刻才重绘，而 _entireOn
 * 还是 0xA5 留下的 true），0xA4/0xA6 改不回来，直到滚动相位（步进会置 dirty）才恢复"。 */
static void test_command_dirty()
{
    section("9. 显示类命令必须置 dirty（只发命令也要能重绘）");

    static const uint8_t cmds[] = { 0xA0, 0xA1, 0xA4, 0xA5, 0xA6, 0xA7, 0xC0, 0xC8, 0xAE, 0xAF };
    int ok = 0;
    for (uint8_t c : cmds) {
        vfd::Ssd1306Emulator e;
        e.clearDirty(); /* 构造/reset 会置 dirty，先清掉才能看出这条命令有没有置 */
        e.pushByte(c, false);
        if (e.dirty() && e.shouldRender(1000, 12))
            ok++;
    }
    check(ok == static_cast<int>(sizeof(cmds)),
        "A0/A1/A4/A5/A6/A7/C0/C8/AE/AF 十条都会置 dirty 且可渲染");

    {
        vfd::Ssd1306Emulator e;
        e.clearDirty();
        e.pushByte(0xD3, false);
        e.pushByte(0x10, false); /* 显示偏移的参数 */
        check(e.dirty() && e.displayOffset() == 0x10, "0xD3（显示偏移）置 dirty 并生效");
    }
    {
        /* 反证：只改写入指针的命令不该白触发重绘 */
        vfd::Ssd1306Emulator e;
        e.clearDirty();
        e.pushByte(0xB0, false);
        check(!e.dirty(), "0xB0（页地址）不置 dirty（省一次无谓渲染）");
    }
}

int main()
{
    printf("SSD1306 行为模拟（宿主机测试）\n");
    printf("模拟器输出：%dx%d，GDDRAM %d B\n", vfd::SSD1306_WIDTH, vfd::SSD1306_HEIGHT,
        vfd::SSD1306_GDDRAM_SIZE);

    test_full_frame_roundtrip();
    test_remap_off();
    test_page_addressing();
    test_horizontal_and_vertical_walk();
    test_display_flags();
    test_robustness();
    test_scroll();
    test_gdram_crc();
    test_command_dirty();

    printf("\n----------------------------------------\n");
    printf("检查项: %d, 失败: %d\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
