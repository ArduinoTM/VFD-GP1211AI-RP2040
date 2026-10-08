/*
 * dump_boot_screen.cpp —— 宿主机侧把"开机自检画面"导出为 ASCII 与 PGM
 *
 * 用途：把固件里 vfd_demo::drawBootScreen() 画出来的帧缓冲（128x64，页式）
 *       落成文本/图像，用于与逻辑分析仪无法覆盖的"实际显示效果"（手机照片）逐点比对。
 *
 * 编译（与 tests/run_host_tests.ps1 相同的选项）：
 *   g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter `
 *       -Isrc -Isrc/compat -Ilib/Adafruit_GFX -Itests `
 *       tests/tools/dump_boot_screen.cpp tests/mock_platform.cpp src/demo_screens.cpp `
 *       src/vfd_gp1211ai.cpp src/vfd_scanpack.cpp src/compat/Print.cpp `
 *       lib/Adafruit_GFX/Adafruit_GFX.cpp -o tests/build/dump_boot_screen.exe
 *
 * 输出：
 *   stdout        ASCII（'#' = 点亮，'.' = 熄灭）
 *   <out>.pgm     128x64 P5 灰度图（255 = 点亮）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo_screens.h"
#include "mock_platform.h"
#include "vfd_gp1211ai.h"

static void dumpAscii(const uint8_t *fb)
{
    for (int y = 0; y < vfd::HEIGHT; ++y) {
        for (int x = 0; x < vfd::WIDTH; ++x) {
            const uint8_t b = fb[(y >> 3) * vfd::WIDTH + x];
            putchar(((b >> (y & 7)) & 1) ? '#' : '.');
        }
        putchar('\n');
    }
}

static void dumpPgm(const uint8_t *fb, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", vfd::WIDTH, vfd::HEIGHT);
    for (int y = 0; y < vfd::HEIGHT; ++y)
        for (int x = 0; x < vfd::WIDTH; ++x) {
            const uint8_t b = fb[(y >> 3) * vfd::WIDTH + x];
            fputc(((b >> (y & 7)) & 1) ? 255 : 0, f);
        }
    fclose(f);
    fprintf(stderr, "已写出 %s\n", path);
}

int main(int argc, char **argv)
{
    MockPlatform plat;
    VFD_GP1211AI display(plat);
    display.begin(0);
    vfd_demo::drawBootScreen(display);

    const uint8_t *fb = display.framebuffer();
    dumpAscii(fb);
    dumpPgm(fb, argc > 1 ? argv[1] : "boot_expected.pgm");

    /* 同时导出"期望 SPI 字节帧"（43×48），供与逻辑分析仪解码结果逐字节对比 */
    {
        static uint8_t frame[vfd::FRAME_SIZE];
        vfd::packFrame(fb, false, frame);
        const char *out = argc > 2 ? argv[2] : "boot_expected.bin";
        FILE *f = fopen(out, "wb");
        if (f) {
            fwrite(frame, 1, sizeof(frame), f);
            fclose(f);
            fprintf(stderr, "已写出期望字节帧 %s（%d 字节 = 43×48）\n", out, (int)sizeof(frame));
        }
    }

    /* 顺便统计点亮像素数，便于与照片解码结果对照 */
    int lit = 0;
    for (int i = 0; i < vfd::FRAMEBUFFER_SIZE; ++i) {
        uint8_t b = fb[i];
        while (b) { lit += (b & 1); b >>= 1; }
    }
    fprintf(stderr, "点亮像素: %d / %d\n", lit, vfd::WIDTH * vfd::HEIGHT);
    return 0;
}
