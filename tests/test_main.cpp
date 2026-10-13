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

    /* 5e. 帧边界等待期间必须给应用层留排水机会（setWaitServiceHook，报告 B P1-2/P2-2）：
     *     引擎停摆时 display() 会一直等到 FRAME_SYNC_TIMEOUT_MS（40 ms）超时；期间钩子应被
     *     反复调用 —— 否则 SSD1306 从机的 16 KB 环形缓冲（4 MHz 下仅 8.2 ms 容量）
     *     会被主机持续送来的数据冲掉（实机观测到一次性 overrun=3 / drop=4263）。 */
    {
        static uint32_t hookCalls = 0;
        MockPlatform plat;
        VFD_GP1211AI display(plat);
        display.begin(0);
        display.display(); /* 先发布一帧，之后 display() 才会进入"等帧边界" */

        hookCalls = 0;
        display.setWaitServiceHook([]() { hookCalls++; });

        plat.setEngineStopped(true); /* 帧计数不再推进 ⇒ 只能等到超时 */
        const uint32_t errorsBefore = display.frameErrorCount();
        display.display();

        check(hookCalls > 0, "帧边界等待期间调用了应用层服务钩子");
        check(display.frameErrorCount() == errorsBefore + 1, "等待超时记为一次帧同步错误");
        printf("  等待超时: 钩子被调用 %lu 次（service 共 %lu 次）\n",
            static_cast<unsigned long>(hookCalls),
            static_cast<unsigned long>(plat.serviceCalls()));
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

/* ------------------------------------------- 8：打包实现（表驱动 gather + 线序折进）
 *
 * 2026-10-13 起 `packScan()` 由"逐位 scatter"改为"表驱动 gather"，tick 的逐字节位反序也折进打包。
 * 这里**保留旧的逐位 scatter 实现作为独立参考**（写法与生产实现完全不同，避免"自己证明自己"），
 * 并把"折进线序"与"两步老路径"逐字节对比。依据：QEMU/Cortex-M0 + `-icount` 台架实测
 * （每帧打包指令数 124.4k → 43.9k 等，见 docs/00 §4.2 的实测表）。 */

static void packScanScatterRef(const uint8_t *fb, int scan, bool invert, uint8_t *out)
{
    memset(out, 0, vfd::SCAN_BYTES);
    if (scan < 0 || scan >= vfd::SCANS_PER_FRAME)
        return;

    const int t = (vfd::SCANS_PER_FRAME - 1) - scan;
    const int x0 = 3 * t;
    const bool t43 = (t == 42);
    const int off0 = t43 ? 2 : ((t & 1) ? 5 : 0);
    const int offStep = t43 ? 2 : ((t & 1) ? -2 : 2);
    const int kShift = t43 ? -2 : 0;
    const int maxDx = t43 ? 2 : 3;

    for (int page = 0; page < vfd::PAGES; ++page) {
        for (int dx = 0; dx < maxDx; ++dx) {
            const int x = x0 + dx;
            if (x < 0 || x >= vfd::WIDTH)
                continue;
            uint8_t src = fb[page * vfd::WIDTH + x];
            if (invert)
                src = static_cast<uint8_t>(~src);
            if (src == 0)
                continue;
            const int off = off0 + offStep * dx;
            for (int m = 0; m < 8; ++m) {
                if (((src >> m) & 1u) == 0)
                    continue;
                const int k = 48 * page + 6 * m + off + kShift;
                if (k < 0 || k >= vfd::SCAN_BYTES * 8)
                    continue;
                out[k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
            }
        }
    }
}

static void packFrameScatterRef(const uint8_t *fb, bool invert, uint8_t *out)
{
    for (int s = 0; s < vfd::SCANS_PER_FRAME; ++s)
        packScanScatterRef(fb, s, invert, out + s * vfd::SCAN_BYTES);
}

/* 6 种画面：全黑 / 全白 / 棋盘 / 随机 / 稀疏 / 每页每列恰好 1 位 */
static void buildTestPicture(uint8_t *fb, int kind)
{
    switch (kind) {
    case 0:
        memset(fb, 0x00, vfd::FRAMEBUFFER_SIZE);
        break;
    case 1:
        memset(fb, 0xFF, vfd::FRAMEBUFFER_SIZE);
        break;
    case 2:
        for (int i = 0; i < vfd::FRAMEBUFFER_SIZE; ++i)
            fb[i] = (i & 1) ? 0xAA : 0x55;
        break;
    case 3:
        for (int i = 0; i < vfd::FRAMEBUFFER_SIZE; ++i)
            fb[i] = static_cast<uint8_t>(rnd());
        break;
    case 4:
        memset(fb, 0, vfd::FRAMEBUFFER_SIZE);
        fb[3 * vfd::WIDTH + 5] = 0x01;
        fb[0 * vfd::WIDTH + 127] = 0x80;
        fb[7 * vfd::WIDTH + 0] = 0xFF;
        break;
    default:
        for (int p = 0; p < vfd::PAGES; ++p)
            for (int x = 0; x < vfd::WIDTH; ++x)
                fb[p * vfd::WIDTH + x] = static_cast<uint8_t>(1u << ((x + p) & 7));
        break;
    }
}

static void test_pack_gather()
{
    section("8. 打包实现：表驱动 gather（vs 逐位 scatter）/ 线序折进 / 边界语义");

    static uint8_t fb[vfd::FRAMEBUFFER_SIZE];
    static uint8_t refOut[vfd::FRAME_SIZE];
    static uint8_t newOut[vfd::FRAME_SIZE];
    static uint8_t twoStep[vfd::FRAME_SIZE];

    const char *picName[6] = { "全黑", "全白", "棋盘", "随机", "稀疏", "每列 1 位" };

    /* 8.1 gather vs 逐位 scatter（6 画面 × invert，逐字节） */
    for (int kind = 0; kind < 6; ++kind) {
        for (int inv = 0; inv < 2; ++inv) {
            buildTestPicture(fb, kind);
            packFrameScatterRef(fb, inv != 0, refOut);
            vfd::packFrame(fb, inv != 0, newOut, false);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s%s：表驱动 gather == 逐位 scatter（2064 B 逐字节）",
                picName[kind], inv ? " + invert" : "");
            check(memcmp(refOut, newOut, vfd::FRAME_SIZE) == 0, msg);
        }
    }

    /* 8.2 线序折进 == 老的两步路径（packFrame(false) + wirePrepareFrame(true)） */
    for (int kind = 0; kind < 6; ++kind) {
        for (int inv = 0; inv < 2; ++inv) {
            buildTestPicture(fb, kind);
            vfd::packFrame(fb, inv != 0, newOut, true); /* 折进：打包时直接产出线序 */
            vfd::wirePrepareFrame(newOut, false);       /* 只做相位 −1 */
            vfd::packFrame(fb, inv != 0, twoStep, false);
            vfd::wirePrepareFrame(twoStep, true);       /* 老路径：反序 + 相位 −1 */
            char msg[112];
            snprintf(msg, sizeof(msg), "%s%s：线序折进 == 两步老路径（含相位 −1）",
                picName[kind], inv ? " + invert" : "");
            check(memcmp(newOut, twoStep, vfd::FRAME_SIZE) == 0, msg);
        }
    }

    /* 8.3 边界与语义 */
    {
        /* T43（scan 0）只写两个槽：a→相对位 0、b→相对位 2，且不得越出 48 bit 组 */
        static uint8_t t43fb[vfd::FRAMEBUFFER_SIZE];
        memset(t43fb, 0, sizeof(t43fb));
        for (int p = 0; p < vfd::PAGES; ++p) {
            t43fb[p * vfd::WIDTH + 126] = 0xFF; /* dx=0 → off 2（kShift −2 后相对位 0） */
            t43fb[p * vfd::WIDTH + 127] = 0xFF; /* dx=1 → off 4（相对位 2） */
        }
        static uint8_t t43Out[vfd::SCAN_BYTES];
        vfd::packScan(t43fb, 0, false, t43Out);
        int okT43 = 1;
        for (int p = 0; p < vfd::PAGES; ++p) {
            for (int m = 0; m < 8; ++m) {
                const int lo = 6 * p + 6 * m; /* 相对位 0 与 2 都置 1 */
                if (((t43Out[lo >> 3] >> (lo & 7)) & 1u) == 0)
                    okT43 = 0;
                const int hi = lo + 2;
                if (((t43Out[hi >> 3] >> (hi & 7)) & 1u) == 0)
                    okT43 = 0;
            }
        }
        check(okT43, "T43 只用 a,b：8 页 × 8 行都在组内相对位 {0,2} 置位");

        int usedBits = 0;
        for (int i = 0; i < vfd::SCAN_BYTES; ++i)
            usedBits += popcountBytes(&t43Out[i], 1);
        check(usedBits == 2 * 8 * 8, "T43 只写两个槽 ⇒ 一扫描恰好 128 位（无填充位被误置）");

        /* 扫描号越界（防御分支）：必须输出全 0，不得越界写 */
        static uint8_t zeroOut[vfd::SCAN_BYTES];
        memset(zeroOut, 0xAA, sizeof(zeroOut));
        vfd::packScan(t43fb, -1, false, zeroOut);
        const bool negOk = (popcountBytes(zeroOut, vfd::SCAN_BYTES) == 0);
        vfd::packScan(t43fb, vfd::SCANS_PER_FRAME, false, zeroOut);
        const bool bigOk = (popcountBytes(zeroOut, vfd::SCAN_BYTES) == 0);
        check(negOk && bigOk, "扫描号越界（−1 / 43）⇒ 输出全 0（防御分支）");

        /* invert 不污染填充位：全黑取反后，非可寻址链位必须仍为 0 */
        static uint8_t blankFb[vfd::FRAMEBUFFER_SIZE];
        vfd::packFrame(blankFb, true, newOut, false);
        int strayInv = 0;
        for (int s = 0; s < vfd::SCANS_PER_FRAME; ++s)
            for (int by = 0; by < vfd::SCAN_BYTES; ++by)
                strayInv += popcountBytes(&newOut[s * vfd::SCAN_BYTES + by], 1);
        check(strayInv == 8192, "全黑 + invert ⇒ 恰好 8192 个可寻址位被置 1（填充位未被污染）");
    }

    /* 8.4 查表与 SWAR 反序的自身一致性 */
    {
        check(vfd::SPREAD.lo[0x01] == 0x00000001u && vfd::SPREAD.lo[0x11] == 0x00000001u
                && vfd::SPREAD.hi[0x11] == 0x00000001u && vfd::SPREAD.hi[0x80] == 0x00040000u,
            "查表语义：bit m → 位 6m（lo 段 m≤3 / hi 段 m≥4）");

        check(vfd::reverseBitsPerByte32(0x01020304u) == 0x8040C020u,
            "SWAR 逐字节反序（4 字节）：0x01020304 → 0x8040C020");

        /* 逐字节反序的两条实现必须一致（32 位版 vs 8 位版） */
        int swarOk = 1;
        for (int v = 0; v < 256; ++v) {
            const uint32_t packed = static_cast<uint32_t>(v) | (static_cast<uint32_t>(v ^ 0xFF) << 8)
                | (static_cast<uint32_t>(v) << 16) | (static_cast<uint32_t>(v ^ 0xFF) << 24);
            const uint32_t rev = vfd::reverseBitsPerByte32(packed);
            if (static_cast<uint8_t>(rev) != vfd::reverseBits8(static_cast<uint8_t>(v))
                || static_cast<uint8_t>(rev >> 8) != vfd::reverseBits8(static_cast<uint8_t>(v ^ 0xFF))
                || static_cast<uint8_t>(rev >> 16) != vfd::reverseBits8(static_cast<uint8_t>(v))
                || static_cast<uint8_t>(rev >> 24) != vfd::reverseBits8(static_cast<uint8_t>(v ^ 0xFF)))
                swarOk = 0;
        }
        check(swarOk, "SWAR 反序与 reverseBits8 逐字节等价（256×4 组合）");
    }
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
    test_pack_gather();

    printf("\n----------------------------------------\n");
    printf("检查项: %d, 失败: %d\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
