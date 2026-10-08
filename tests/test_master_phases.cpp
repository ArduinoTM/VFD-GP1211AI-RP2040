/*
 * 宿主机测试：把主控"完整测试"程序（examples/pico2_full_test）的**相位序列**在 PC 上回放一遍
 *
 * 为什么需要它：pico2_full_test 的相位是"契约"—— 每个相位发什么命令/多少字节、从机的 GDDRAM
 * 应当变成什么（CRC32），都写在 docs/05 §8.6 与 docs/10 §5 的验收表里。主控程序改了相位，
 * 这里就必须同步改；反过来，模拟器改坏了哪条寻址路径，这里会立刻红。
 *
 * 覆盖：初始化命令序列 → 整屏页寻址 → 整屏水平寻址 → 整屏垂直寻址（列优先数据）→
 *       局部窗口 → 反显/全亮/显示开关 → 对比度扫描 → 四种滚动 → 30 帧压力。
 * 这里只喂"命令/数据流"，不涉及 SPI/PIO（那部分由 tests/test_ssd1306_pio.cpp 与固件构建覆盖）。
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

/* ------------------------------------------------------------ 主控相位回放 */

static vfd::Ssd1306Emulator emu;
static uint8_t img[1024], imgCol[1024], expected[1024], fb[1024];
static uint32_t gCmd = 0, gData = 0;

static const uint32_t REF_IMAGE_CRC = 0xCCE79D14u;
static const uint32_t BLANK_CRC = 0xEFB5AF2Eu; /* CRC32(1024 个 0x00) */

static void cmd(uint8_t c) { emu.pushByte(c, false); gCmd++; }
static void cmds(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) cmd(p[i]); }
static void data(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { emu.pushByte(p[i], true); gData++; } }

/* 渲染一遍并数点亮像素（= 面板上真正会亮的点数） */
static int litPixels()
{
    emu.renderToFramebuffer(fb);
    int lit = 0;
    for (int i = 0; i < 1024; ++i)
        lit += __builtin_popcount(fb[i]);
    return lit;
}

static void buildImage()
{
    for (int p = 0; p < 8; ++p) {
        for (int c = 0; c < 128; ++c) {
            uint8_t v;
            if (p == 0 || p == 7 || c == 0 || c == 127) v = 0xFF;
            else if ((((c >> 3) + (p >> 1)) & 1) != 0) v = 0x55;
            else v = (uint8_t)(c * 7 + p * 31 + 0x5A);
            img[p * 128 + c] = v;
        }
    }
    for (int c = 0; c < 128; ++c)
        for (int p = 0; p < 8; ++p)
            imgCol[c * 8 + p] = img[p * 128 + c];
}

int main()
{
    printf("主控相位序列回放（examples/pico2_full_test 的契约测试）\n");

    buildImage();
    check(vfd::crc32(img, sizeof(img)) == REF_IMAGE_CRC, "参考图 CRC = 0xCCE79D14（与主控/Node 一致）");

    /* ---------------------------------------------------------------- §1 */
    section("§1 启动 + 初始化序列");
    {
        check(emu.gdramCrc32() == BLANK_CRC, "上电 GDDRAM = 1024 个 0x00（CRC 不是 0x00000000，而是 0xEFB5AF2E）");
        check(litPixels() == 0, "上电全黑");

        static const uint8_t INIT[] = { 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
            0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF };
        cmds(INIT, sizeof(INIT));
        check(sizeof(INIT) == 25, "初始化序列 25 字节（验收表：cmd +25）");
        check(emu.displayOn(), "disp=1");
        check(emu.contrast() == 0xCF, "contrast=207（0xCF）");
        check(emu.addressingMode() == vfd::Ssd1306Emulator::ADDR_HORIZONTAL, "寻址模式 = 水平（0x20 0x00）");
        check(emu.gdramCrc32() == BLANK_CRC, "初始化不改 GDDRAM");
        check(emu.unknownCommandCount() == 0, "初始化序列无未知命令");
    }

    /* ---------------------------------------------------------------- §2 */
    section("§2 三种寻址模式写同一幅图 ⇒ CRC 必须一致");
    int litRef = 0;
    {
        /* 页寻址：8 × (0xB0+p, 0x00, 0x10 + 128 B) */
        for (uint8_t p = 0; p < 8; ++p) {
            const uint8_t c3[3] = { (uint8_t)(0xB0 | p), 0x00, 0x10 };
            cmds(c3, 3);
            data(&img[p * 128], 128);
        }
        check(emu.gdramCrc32() == REF_IMAGE_CRC, "页寻址后 = 参考图");
        litRef = litPixels();
        check(litRef > 4000 && litRef < 6000, "参考图点亮数在合理范围（约 5157）");

        /* 水平寻址：0x20 0x00 + 窗口 + 1024 B */
        const uint8_t c4[] = { 0x20, 0x00, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
        cmds(c4, sizeof(c4));
        data(img, sizeof(img));
        check(emu.gdramCrc32() == REF_IMAGE_CRC, "水平寻址后仍 = 参考图（屏幕不应变空！）");
        check(litPixels() == litRef, "水平寻址后点亮数不变");

        /* 垂直寻址：同样的窗口，但数据必须"列优先" */
        const uint8_t c5[] = { 0x20, 0x01, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
        cmds(c5, sizeof(c5));
        data(imgCol, sizeof(imgCol));
        check(emu.gdramCrc32() == REF_IMAGE_CRC, "垂直寻址（列优先数据）后 = 参考图");
        check(emu.addressingMode() == vfd::Ssd1306Emulator::ADDR_VERTICAL, "寻址模式 = 垂直");
    }

    /* ---------------------------------------------------------------- §3 */
    section("§3 局部窗口 + 显示语义");
    uint32_t crcWindow = 0;
    {
        const uint8_t c6[] = { 0x20, 0x00, 0x21, 0x20, 0x5F, 0x22, 0x02, 0x05 };
        cmds(c6, sizeof(c6));
        uint8_t win[256];
        for (int i = 0; i < 256; ++i) win[i] = (uint8_t)(0xAA ^ (i * 3));
        memcpy(expected, img, sizeof(expected));
        int k = 0;
        for (int p = 2; p <= 5; ++p)
            for (int c = 0x20; c <= 0x5F; ++c)
                expected[p * 128 + c] = win[k++];
        data(win, sizeof(win));

        crcWindow = vfd::crc32(expected, sizeof(expected));
        check(emu.gdramCrc32() == crcWindow, "局部窗口写入 = 参考图 + 窗口图案（窗口外不变）");
        check(crcWindow != REF_IMAGE_CRC, "窗口确实改动了 GDDRAM");

        /* 反显 / 全亮 / 显示开关：只影响渲染，不改 GDDRAM。
         * 与主控的"逐步保持 0.8 s"一致，这里**逐条**应用并检查**渲染结果**（不只是标志位）——
         * 这也是回答"背靠背发完为什么屏幕没变化"的自动化版本：净状态与之前相同。 */
        uint8_t baseFb[1024];
        emu.renderToFramebuffer(baseFb);

        cmd(0xA7);
        check(emu.inverse(), "0xA7 ⇒ 反显标志置位");
        check(emu.dirty(), "0xA7 置 dirty ⇒ 应用会重绘（否则屏幕上根本看不到反显）");
        emu.renderToFramebuffer(fb);
        {
            int diff = 0;
            for (int i = 0; i < 1024; ++i)
                if (fb[i] != (uint8_t)~baseFb[i]) diff++;
            check(diff == 0, "0xA7 的渲染结果 = 正常显示的逐位取反（肉眼可见的明暗翻转）");
        }

        cmd(0xA5);
        check(emu.entireDisplayOn(), "0xA5 ⇒ 全亮标志置位");
        check(emu.dirty(), "0xA5 置 dirty ⇒ 应用会重绘");
        emu.renderToFramebuffer(fb);
        {
            int full = 0;
            for (int i = 0; i < 1024; ++i) if (fb[i] == 0xFF) full++;
            check(full == 1024, "0xA5 的渲染结果 = 整屏点亮（忽略 GDDRAM）");
        }

        cmd(0xAE);
        check(!emu.displayOn(), "0xAE ⇒ 关显示");
        check(emu.dirty(), "0xAE 置 dirty ⇒ 应用会重绘（全黑）");
        emu.renderToFramebuffer(fb);
        {
            int dark = 0;
            for (int i = 0; i < 1024; ++i) if (fb[i] == 0x00) dark++;
            check(dark == 1024, "0xAE 的渲染结果 = 全黑");
        }

        cmd(0xAF);
        check(emu.dirty(), "0xAF 置 dirty：此刻 _entireOn 仍为真 ⇒ 会渲染成全亮（正是现场那一步）");
        cmd(0xA4);
        check(!emu.entireDisplayOn() && emu.dirty(), "0xA4 清全亮并置 dirty（能改回来了）");
        cmd(0xA6);
        check(!emu.inverse() && emu.dirty(), "0xA6 清反显并置 dirty");
        check(emu.displayOn() && !emu.entireDisplayOn() && !emu.inverse(),
            "0xAF/0xA4/0xA6 ⇒ 回到正常显示（与相位开始时相同 ⇒ 背靠背发完看不出变化是正常的）");
        check(emu.gdramCrc32() == crcWindow, "这一组命令不改 GDDRAM（CRC 不变）");
    }

    /* ---------------------------------------------------------------- §4 */
    section("§4 对比度扫描（0x81 九档）");
    {
        /* 与主控 phContrastSweep 一致：最后一档必须是 0xFF，否则会"把亮度留在 0" */
        static const uint8_t levels[9] = { 0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0xFF };
        for (int i = 0; i < 9; ++i) {
            const uint8_t c8[2] = { 0x81, levels[i] };
            cmds(c8, 2);
        }
        check(emu.gdramCrc32() == crcWindow, "对比度不改 GDDRAM");
        check(emu.contrast() == 0xFF, "扫描结束时 contrast=255（最亮）—— 现场那次停在 0 是主控档位溢出（见 docs/00 陷阱 32）");
        check(!((uint8_t)(8 * 0x20) == 0xFF), "反例：旧的 (uint8_t)(i*0x20) 在 i=8 时溢出为 0x00");
    }

    /* ---------------------------------------------------------------- §5 */
    section("§5 四种滚动 + 收尾重画");
    {
        static const uint8_t modes[4] = { 0x26, 0x27, 0x29, 0x2A };
        uint32_t now = 1000;
        int changed = 0;
        for (int m = 0; m < 4; ++m) {
            const uint8_t set[7] = { modes[m], 0x00, 0x00, 0x07, 0x07, 0x00, 0xFF };
            cmds(set, sizeof(set));
            cmd(0x2F); /* 启动 */
            const uint32_t crcBefore = emu.gdramCrc32();
            for (int k = 0; k < 75; ++k) { now += 16; emu.tick(now); }
            if (emu.gdramCrc32() != crcBefore) changed++;
            check(emu.scrollStepCount() > 0, "滚动步数增长");
            cmd(0x2E); /* 停止 */
        }
        check(changed >= 3, "四种滚动都改写了 GDDRAM（CRC 变化）");
        check(!emu.scrollActive(), "0x2E 之后滚动停止");

        /* 收尾：主控用 30 帧水平寻址把画面重画成参考图 */
        const uint8_t c10[] = { 0x20, 0x00, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
        for (int f = 0; f < 30; ++f) {
            cmds(c10, sizeof(c10));
            data(img, sizeof(img));
        }
        check(emu.gdramCrc32() == REF_IMAGE_CRC, "30 帧压力后又回到参考图（滚动造成的错位被重画覆盖）");
        check(litPixels() == litRef, "点亮数与最初一致 ⇒ 没有残留错位");
        check(emu.unknownCommandCount() == 0, "全程没有未知命令");
    }

    printf("\n----------------------------------------\n");
    printf("检查项: %d, 失败: %d\n", g_checks, g_failures);
    printf("回放累计: cmd=%lu data=%lu\n", (unsigned long)gCmd, (unsigned long)gData);
    printf("%s\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
