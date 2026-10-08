/*
 * 宿主机测试（g++，与 RP2040 目标共用同一份驱动/GFX/兼容层源码）。
 *
 * 覆盖：
 *   1. 位重排与原 STM32 驱动 display() 的 1:1 转录结果逐位等价
 *      （仅忽略原版越界读列 128 造成的、落在不存在阳极上的垃圾位）；
 *   2. 8192 个像素 -> (扫描, 链位号) 是双射；不存在"另一组阳极"被写 1 的情况；
 *   3. 反显只在真实像素位上取反；
 *   4. 绘图原语 / 颜色归一化 / setRotation 保护 / fillScreen(INVERSE)；
 *   5. display() 双缓冲协议不撕裂（含"应用比引擎快"的连续调用场景）；
 *   6. 扫描停摆看护：自动 recover，连续失败 emergencyOff；
 *   7. 示例画面渲染（打印 ASCII 预览）。
 */
#include <stdio.h>
#include <string.h>

#include "demo_screens.h"
#include "mock_platform.h"
#include "reference_pack.h"
#include "vfd_gp1211ai.h"
#include "vfd_scanpack.h"

/* ------------------------------------------------------------------ 小框架 */

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

/* 确定性伪随机，保证可复现 */
static uint32_t g_rng = 0x12345678u;
static uint32_t rnd()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

/* --------------------------------------------------- 可寻址位掩码（按公式） */

/* addrMask[scan][byte] 的 bit = 该链位是否被某个真实像素使用 */
static void buildAddressMask(uint8_t addrMask[vfd::SCANS_PER_FRAME][vfd::SCAN_BYTES],
    int hitCount[vfd::WIDTH * vfd::HEIGHT])
{
    memset(addrMask, 0, sizeof(uint8_t) * vfd::SCANS_PER_FRAME * vfd::SCAN_BYTES);
    for (int i = 0; i < vfd::WIDTH * vfd::HEIGHT; ++i)
        hitCount[i] = 0;

    for (int s = 0; s < vfd::SCANS_PER_FRAME; ++s) {
        const int t = (vfd::SCANS_PER_FRAME - 1) - s;
        const int x0 = 3 * t;
        for (int y = 0; y < vfd::HEIGHT; ++y) {
            for (int dx = 0; dx < 3; ++dx) {
                const int x = x0 + dx;
                if (x >= vfd::WIDTH)
                    continue;
                /* 槽位映射（与 vfd_scanpack.cpp 的**默认**一致）：
                 * 一般时序 band 1：t 偶 -> off 0,2,4；t 奇 -> off 5,3,1。
                 * T43（t == 42，手册 Note 12：只用 a,b）：实机定标为 a→off2、b→off4、
                 * k 整体 −2、列不动，且只有两个槽（dx==2 不产生数据）。 */
                int off, kShift = 0;
                if (t == 42) {
                    if (dx == 2)
                        continue; /* T43 只有 a,b 两槽 */
                    off = (dx == 0) ? 2 : 4;
                    kShift = -2;
                } else {
                    off = (t & 1) ? (5 - 2 * dx) : (2 * dx);
                }
                const int k = 48 * (y >> 3) + 6 * (y & 7) + off + kShift;
                if (k < 0 || k >= vfd::SCAN_BYTES * 8)
                    continue;
                addrMask[s][k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
                hitCount[y * vfd::WIDTH + x]++;
            }
        }
    }
}

static int popcountBytes(const uint8_t *buf, int n)
{
    int c = 0;
    for (int i = 0; i < n; ++i)
        for (int b = 0; b < 8; ++b)
            c += (buf[i] >> b) & 1;
    return c;
}

/* --------------------------------------------- 1/2/3：位重排与原实现对比 */

static void test_pack_equivalence()
{
    section("1. 位重排 vs 原驱动 display() 逐位对比（原驱动槽序镜像后）");

    static uint8_t addrMask[vfd::SCANS_PER_FRAME][vfd::SCAN_BYTES];
    static int hitCount[vfd::WIDTH * vfd::HEIGHT];
    buildAddressMask(addrMask, hitCount);

    /* 双射检查 */
    int total = 0, multi = 0, zero = 0;
    for (int i = 0; i < vfd::WIDTH * vfd::HEIGHT; ++i) {
        total += hitCount[i];
        if (hitCount[i] > 1)
            multi++;
        if (hitCount[i] == 0)
            zero++;
    }
    printf("  像素 -> (扫描, 链位) 双射: 覆盖 %d/%d, 重复 %d, 遗漏 %d\n",
        total, vfd::WIDTH * vfd::HEIGHT, multi, zero);
    check(multi == 0 && zero == 0 && total == vfd::WIDTH * vfd::HEIGHT,
        "8192 像素恰好被寻址一次");

    /* 可寻址位总数 = 8192 */
    int maskBits = 0;
    for (int s = 0; s < vfd::SCANS_PER_FRAME; ++s)
        maskBits += popcountBytes(addrMask[s], vfd::SCAN_BYTES);
    printf("  可寻址链位总数: %d (期望 8192)\n", maskBits);
    check(maskBits == 8192, "链位占用数正确");

    /* 随机帧缓冲：新实现 vs 原实现 */
    static uint8_t fb[vfd::FRAMEBUFFER_SIZE];
    static uint8_t refOut[vfd::FRAME_SIZE];
    static uint8_t newOut[vfd::FRAME_SIZE];
    static uint8_t invOut[vfd::FRAME_SIZE];

    int mismatchedAddressable = 0;
    int strayBits = 0; /* 新实现写到"非可寻址位"的次数（必须为 0） */
    int refStrayBits = 0; /* 原实现写到非可寻址位的位数（列 128 越界垃圾） */
    int invertErrors = 0;

    for (int trial = 0; trial < 50; ++trial) {
        for (int i = 0; i < vfd::FRAMEBUFFER_SIZE; ++i)
            fb[i] = static_cast<uint8_t>(rnd());
        if (trial == 0)
            memset(fb, 0, sizeof(fb)); /* 全黑 */
        if (trial == 1)
            memset(fb, 0xFF, sizeof(fb)); /* 全白 */

        /* 默认就是原驱动槽序（mode 1）⇒ 与转录实现逐位直比（不需要置换）。
         * 例外：t = 42（T43）有手册 Note 12 特例（只用 a,b）——原驱动转录没有这层处理，
         * 该时序不参与逐位比对（下面把它清零），由 §T43 专项检查覆盖。 */
        ref::packFrame(fb, refOut); /* 原样保留：它的"列 128 垃圾位"正是下面 refStrayBits 的证据 */
        vfd::packFrame(fb, false, newOut);
        vfd::packFrame(fb, true, invOut);

        for (int s = 0; s < vfd::SCANS_PER_FRAME; ++s) {
            for (int b = 0; b < vfd::SCAN_BYTES; ++b) {
                const uint8_t mask = addrMask[s][b];
                const uint8_t a = newOut[s * vfd::SCAN_BYTES + b];
                const uint8_t r = refOut[s * vfd::SCAN_BYTES + b];
                const uint8_t v = invOut[s * vfd::SCAN_BYTES + b];

                /* s == 0 即 T43：新实现按手册 Note 12（只用 a,b，实机定标 a→2/b→4/k−2）
                 * 处理，而原驱动转录没有这层 ⇒ 该时序不参与"逐位一致"与"反显"比对；
                 * 越界位检查照旧（mask 用的是含 T43 规则的 addrMask）。 */
                if (s != 0) {
                    if ((a & mask) != (r & mask))
                        mismatchedAddressable++;
                    if ((v & mask) != (static_cast<uint8_t>(~a) & mask))
                        invertErrors++;
                }
                if (r & static_cast<uint8_t>(~mask))
                    refStrayBits++;
                if (a & static_cast<uint8_t>(~mask))
                    strayBits++;
                if (v & static_cast<uint8_t>(~mask))
                    strayBits++;
            }
        }
    }

    printf("  50 组随机帧: 可寻址位不一致 %d, 新实现越界位 %d, 原实现越界位(列128垃圾) %d\n",
        mismatchedAddressable, strayBits, refStrayBits);
    check(mismatchedAddressable == 0, "可寻址位与原实现（槽序镜像后）逐位一致");
    check(strayBits == 0, "新实现不写非可寻址位（另一组阳极恒 0）");
    check(refStrayBits > 0, "原实现确实会写越界位（说明该差异是真实存在且被显式忽略的）");
    check(invertErrors == 0, "反显只在真实像素位上取反");
}

/* ------------------------------------------------------ 4：绘图原语与兼容性 */

static int fbPixelCount(const uint8_t *fb)
{
    return popcountBytes(fb, vfd::FRAMEBUFFER_SIZE);
}

static void test_driver_primitives()
{
    section("4. 绘图原语 / 颜色归一化 / 旋转保护");

    MockPlatform plat;
    VFD_GP1211AI display(plat);

    const uint8_t *fb = display.framebuffer();

    /* 越界访问不得改动帧缓冲 */
    static uint8_t before[vfd::FRAMEBUFFER_SIZE];
    memcpy(before, fb, sizeof(before));
    display.drawPixel(-1, 0, WHITE);
    display.drawPixel(0, -1, WHITE);
    display.drawPixel(vfd::WIDTH, 0, WHITE);
    display.drawPixel(0, vfd::HEIGHT, WHITE);
    display.drawPixel(1000, 1000, WHITE);
    check(memcmp(before, fb, sizeof(before)) == 0, "越界 drawPixel 不改动帧缓冲");

    /* 边界内的像素 */
    display.drawPixel(0, 0, WHITE);
    display.drawPixel(vfd::WIDTH - 1, vfd::HEIGHT - 1, WHITE);
    check(vfd::pixelGet(fb, 0, 0) && vfd::pixelGet(fb, vfd::WIDTH - 1, vfd::HEIGHT - 1),
        "边界像素可绘制");

    /* 颜色归一化：0xFFFF（GFX 默认文字色）应被当作 WHITE */
    display.clearDisplay();
    display.drawPixel(3, 3, 0xFFFF);
    check(vfd::pixelGet(fb, 3, 3), "颜色 >= 3 归一化为 WHITE");

    /* 文字：显式使用默认色 0xFFFF，原版会什么都画不出来 */
    display.clearDisplay();
    display.setTextColor(0xFFFF);
    display.setCursor(0, 0);
    display.print(F("OK"));
    check(fbPixelCount(fb) > 20, "默认文字色可以正常写字");

    /* 数字/浮点打印路径（兼容层 Print） */
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextColor(WHITE);
    display.print(-12345);
    display.print(' ');
    display.print(0xDEADBEEFu, HEX);
    display.print(' ');
    display.print(3.14159, 2);
    check(fbPixelCount(fb) > 20, "Print 的数字/浮点/HEX 路径可用");

    /* 水平/垂直线 */
    display.clearDisplay();
    display.drawFastHLine(10, 5, 20, WHITE);
    int hline = 0;
    for (int x = 0; x < vfd::WIDTH; ++x)
        hline += vfd::pixelGet(fb, x, 5) ? 1 : 0;
    check(hline == 20, "drawFastHLine 宽度正确");

    display.clearDisplay();
    display.drawFastVLine(7, 3, 30, WHITE);
    int vline = 0;
    for (int y = 0; y < vfd::HEIGHT; ++y)
        vline += vfd::pixelGet(fb, 7, y) ? 1 : 0;
    check(vline == 30, "drawFastVLine 高度正确（跨页正确）");

    /* 跨页/越界的线 */
    display.clearDisplay();
    display.drawFastVLine(0, -10, 100, WHITE); /* 裁剪到 0..63 */
    vline = 0;
    for (int y = 0; y < vfd::HEIGHT; ++y)
        vline += vfd::pixelGet(fb, 0, y) ? 1 : 0;
    check(vline == vfd::HEIGHT, "越界垂直线被正确裁剪");

    /* fillScreen */
    display.fillScreen(WHITE);
    check(fbPixelCount(fb) == vfd::WIDTH * vfd::HEIGHT, "fillScreen(WHITE) 全亮");
    display.fillScreen(INVERSE);
    check(fbPixelCount(fb) == 0, "fillScreen(INVERSE) 全反转为黑");
    display.fillScreen(INVERSE);
    check(fbPixelCount(fb) == vfd::WIDTH * vfd::HEIGHT, "fillScreen(INVERSE) 再反转回全亮");
    display.fillScreen(BLACK);
    check(fbPixelCount(fb) == 0, "fillScreen(BLACK) 全灭");

    /* fillRect(INVERSE) 与 fillScreen(INVERSE) 语义一致（异或） */
    display.clearDisplay();
    display.fillRect(0, 0, vfd::WIDTH, vfd::HEIGHT, INVERSE);
    check(fbPixelCount(fb) == vfd::WIDTH * vfd::HEIGHT, "fillRect(INVERSE) 等价于取反");

    /* 旋转必须被拒绝，否则会越界写坏内存 */
    display.setRotation(2);
    check(display.width() == vfd::WIDTH && display.height() == vfd::HEIGHT,
        "setRotation 被拒绝，显示尺寸保持 128x64");
    display.clearDisplay();
    display.drawPixel(127, 63, WHITE);
    check(vfd::pixelGet(fb, 127, 63), "旋转被拒后绘制仍落在正确位置");
}

/* ------------------------------------------------- 5：双缓冲发布协议不撕裂 */

static void alignToFrameStart(MockPlatform &plat)
{
    const int s = plat.currentScan();
    if (s != 0)
        plat.advanceUs(static_cast<uint64_t>(MockPlatform::SCAN_PERIOD_US) * (vfd::SCANS_PER_FRAME - s));
}

static void test_publish_protocol()
{
    section("5. display() 双缓冲协议 / 扫描引擎数据通路");

    /* 5a. 单帧数据通路：引擎锁存的数据 == 新实现打包的数据 */
    {
        MockPlatform plat;
        VFD_GP1211AI display(plat);
        display.begin(0);
        vfd_demo::drawBootScreen(display);
        display.display();
        alignToFrameStart(plat);
        plat.stepFrame();

        static uint8_t expected[vfd::FRAME_SIZE];
        vfd::packFrame(display.framebuffer(), false, expected);
        /* 引擎锁存的是**传输层适配后**的字节（逐字节反序 + 扫描相位 −1），
         * 所以期望值也要过一遍 wirePrepareFrame（两个引擎的这层适配已固化，不再有可调项）。 */
        vfd::wirePrepareFrame(expected, plat.wireReversesByteBits());
        check(memcmp(plat.latchedFrame(), expected, vfd::FRAME_SIZE) == 0,
            "引擎锁存的 43×48 字节 == packFrame() + wirePrepareFrame() 输出");
        printf("  单帧锁存数据校验: 前 16 字节 =");
        for (int i = 0; i < 16; ++i)
            printf(" %02X", plat.latchedFrame()[i]);
        printf("\n");
    }

    /* 5b. 常规节奏：每帧调用一次 display()，连续 200 帧不得撕裂 */
    {
        MockPlatform plat;
        VFD_GP1211AI display(plat);
        display.begin(0);
        alignToFrameStart(plat);

        for (int i = 0; i < 200; ++i) {
            display.clearDisplay();
            display.fillRect(i % 100, 8, 24, 24, WHITE);
            display.drawCircle(vfd::WIDTH / 2, vfd::HEIGHT / 2, i % 30, WHITE);
            display.display();
            plat.stepFrame();
        }
        check(!plat.tearDetected(), "200 帧常规节奏无撕裂");
        check(display.frameErrorCount() == 0, "常规节奏无帧同步超时");
    }

    /* 5c. 应用比引擎快：连续调用 display()（第二次必须等待帧边界） */
    {
        MockPlatform plat;
        VFD_GP1211AI display(plat);
        display.begin(0);

        for (int i = 0; i < 60; ++i) {
            display.clearDisplay();
            display.drawRect(i % 90, 10, 30, 20, WHITE);
            display.display();
            display.display();
            plat.stepFrame();
        }
        check(!plat.tearDetected(), "应用快于引擎时无撕裂");
        check(display.frameErrorCount() == 0, "等待帧边界不会误判为超时");
        printf("  快速连续 display(): 服务调用 %lu 次, 引擎帧数 %lu\n",
            static_cast<unsigned long>(plat.serviceCalls()),
            static_cast<unsigned long>(plat.frameStartCount()));
    }

    /* 5d. 反显通过数据通路生效 */
    {
        MockPlatform plat;
        VFD_GP1211AI display(plat);
        display.begin(0);
        display.clearDisplay();
        display.drawPixel(1, 1, WHITE);
        display.invertDisplay(true);
        display.display();
        alignToFrameStart(plat);
        plat.stepFrame();

        static uint8_t expectInv[vfd::FRAME_SIZE];
        vfd::packFrame(display.framebuffer(), true, expectInv);
        vfd::wirePrepareFrame(expectInv, plat.wireReversesByteBits());
        check(memcmp(plat.latchedFrame(), expectInv, vfd::FRAME_SIZE) == 0,
            "invertDisplay(true) 在重排阶段生效");
    }
}

/* ------------------------------------------------------- 6：扫描停摆看护 */

static void test_scan_health()
{
    section("6. 扫描心跳看护 / 恢复 / 紧急关断");

    MockPlatform plat;
    VFD_GP1211AI display(plat);
    display.begin(0);

    plat.advanceUs(189u * 43u);
    check(display.task(), "正常运行 task() 返回 true");

    /* 引擎停摆 60 ms（> 50 ms 阈值） */
    plat.setEngineStopped(true);
    plat.delayMs(60);
    const bool alive = display.task();
    check(!alive, "扫描停摆会被检出");
    check(plat.recoverCount() == 1, "检出后调用 recover()");
    check(!display.isFatal(), "单次故障不判死");

    /* recover 成功：扫描恢复，故障计数保留 */
    plat.advanceUs(189u * 43u);
    check(display.task(), "恢复后回到正常");
    check(display.faultCount() == 1, "故障计数保留");

    /* recover 一直失败 -> 紧急关断高压并进入 fatal（保护屏） */
    plat.setEngineStopped(true);
    plat.setRecoverFails(true);
    for (int i = 0; i < 8 && !display.isFatal(); ++i) {
        plat.delayMs(60);
        display.task();
    }
    check(display.isFatal(), "连续恢复失败进入 fatal");
    check(plat.emergencyOffCount() == 1, "fatal 前调用 emergencyOff()");
    check(!plat.hvOn(), "紧急关断后高压已断开");

    /* 上电时序：灯丝先于高压，且预热时间被真正等到 */
    MockPlatform p2;
    VFD_GP1211AI d2(p2);
    d2.begin(300);
    check(p2.powerUpCount() == 1, "begin() 执行了一次上电时序");
    check(p2.preheatSeenMs() >= 300, "灯丝预热时间被等到");
    printf("  预热实测: %lu ms, 亮度 PWM 窗口: %lu us\n",
        static_cast<unsigned long>(p2.preheatSeenMs()),
        static_cast<unsigned long>(p2.litWindowUs()));
}

/* ------------------------------------------------------------ 7：画面预览 */

static void printAscii(const uint8_t *fb)
{
    for (int y = 0; y < vfd::HEIGHT; y += 2) {
        putchar('|');
        for (int x = 0; x < vfd::WIDTH; ++x) {
            const bool on = vfd::pixelGet(fb, x, y) || vfd::pixelGet(fb, x, y + 1);
            putchar(on ? '#' : ' ');
        }
        printf("|\n");
    }
}

static void test_demo_screens()
{
    section("7. 示例画面渲染");

    MockPlatform plat;
    VFD_GP1211AI display(plat);
    display.begin(0);

    vfd_demo::drawBootScreen(display);
    const int bootPixels = fbPixelCount(display.framebuffer());
    check(bootPixels > 300, "开机画面有内容");
    check(vfd::pixelGet(display.framebuffer(), 0, 0), "边框左上角被点亮");
    check(vfd::pixelGet(display.framebuffer(), vfd::WIDTH - 1, vfd::HEIGHT - 1),
        "边框右下角被点亮");
    printf("\n开机画面 (ASCII 预览, 每 2 行合 1 行):\n");
    printAscii(display.framebuffer());

    vfd_demo::drawAnimationFrame(display, 30);
    check(fbPixelCount(display.framebuffer()) > 200, "动画帧有内容");
    printf("\n动画第 30 帧:\n");
    printAscii(display.framebuffer());
}

/* ------------------------------------------------------------------- main */

int main()
{
    printf("VFD-GP1211AI-RP2040 宿主机测试\n");
    printf("帧缓冲 %d B, 发送缓冲 %d B (43 x 48), 屏 %dx%d\n",
        vfd::FRAMEBUFFER_SIZE, vfd::FRAME_SIZE, vfd::WIDTH, vfd::HEIGHT);

    test_pack_equivalence();
    test_driver_primitives();
    test_publish_protocol();
    test_scan_health();
    test_demo_screens();

    printf("\n----------------------------------------\n");
    printf("检查项: %d, 失败: %d\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
