/*
 * due_full_test.ino —— SSD1306 模拟器"完整测试"主控端（Arduino Due → GP1211AI）
 *
 * 这是 examples/pico2_full_test 的 **Arduino Due 移植版**：功能、相位、判定方法完全一致
 * （分相位覆盖 + 两端 cmd/data/gdram_crc 对照，不需要相机/示波器）。
 *
 * ========================= 接线（Arduino Due → 模拟器 RP2040）=========================
 *
 *   Due 侧              模拟器 RP2040      说明
 *   ------------------  ----------------   --------------------------------------------
 *   ICSP-3 (SCK,  76) → GP11 SCLK          硬件 SPI 时钟；**只能从这个 2×3 排针引**
 *   ICSP-4 (MOSI, 75) → GP12 MOSI          硬件 SPI 数据；**只能从这个 2×3 排针引**
 *   D22               → GP13 DC            普通 GPIO（本程序用 22）
 *   D24               → GP14 CS            普通 GPIO（本程序用 24，低有效）
 *   D26               → GP15 RESET         普通 GPIO（可省：不接就改成 U8X8_PIN_NONE）
 *   GND               → GND                **必须共地**
 *   （模拟器 GP16 上电为高/悬空 ⇒ SSD1306 模式）
 *
 * ⚠️ Due 与 Pico 的三处关键差异（照抄 pico2 版会出问题）：
 *
 * 1. **SPI 引脚只在 ICSP 2×3 排针上**（MISO=ICSP-1 / SCK=ICSP-3 / MOSI=ICSP-4），
 *    数字排针上**没有** SPI。别去数字口找 11/12/13 —— 那是 Uno 的排法；Due 上的
 *    11/12/13 是普通 GPIO（13 还挂着板载 LED）。
 *    注：Due 的 SPI 引脚定义是 PIN_SPI_SCK=76 / PIN_SPI_MOSI=75（variant.h），
 *    排针上的丝印是 ICSP-3 / ICSP-4。
 *
 * 2. **Due 的 SPI 库没有"只发不收"的块传输**：
 *      · `SPI.transfer(void*, size_t)` 是**原地**的 —— 收到的字节会覆写发送缓冲区
 *        （库自己的注释：the buffer is overwritten with the incoming data）；
 *      · `SPI.transfer(tx, rx, n)` 这个重载在 SAM 核里**根本不存在**（那是 ESP/新核 API）。
 *    所以数据发送必须**逐字节** `SPI.transfer(b)`（u8g2 的 Arduino 硬件 SPI 也是这么做的，
 *    见 U8x8lib.cpp 里那段注释）。**别为了快一点改回块传输** —— 那会把 gImg 原地覆盖成
 *    0x00，后续相位全发 0、期望 CRC 也跟着错。
 *
 * 3. **电平**：Due 是 3.3 V 逻辑 ⇒ 与 RP2040 **可以直连，不需要电平转换**
 *    （Uno/Nano 那类 5 V 板才需要；Due 不适用）。
 *
 * 串口：Due 的 `Serial` 走 **native USB 口**（靠复位键那个），不是 programming 口；
 *       USB CDC 下波特率无意义，保留 115200 只为与 pico2 版一致。
 *
 * 串口交互：p 暂停/继续自动推进 · n 下一相位 · r 重跑本相位 · a 从头发跑 · h 帮助
 *
 * 判读（两端对照）：从机每秒那一行形如
 *   `rx=… cmd=…(+x) data=…(+y) … gdram_crc=0xZZZZZZZZ`
 * 与本程序每相位打印的 `expect: slave cmd=… data=… gdram_crc=0x…` 对齐即可。
 * 注意：滚动相位会改写 GDDRAM，CRC 不可预测（判据是 steps>0 且数字在变）；其余相位都可对。
 *
 * 备注：Due 是 84 MHz Cortex-M3，逐字节发送比 Pico 慢（4 MHz 下 1024 B 约数 ms）；
 *       判据不依赖时间，只影响观感。
 */

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ 接线/参数 */
/* Due 的硬件 SPI 引脚固定在 ICSP 排针上（SCK=76 / MOSI=75），**不可改**；
 * CS/DC/RESET 用普通 GPIO —— 本程序选 22/24/26。
 * ⚠️ 避开：10 / 4 / 52 / 78（Due 的 SPI NPCS 片选引脚）、20 / 21（TWI）、13（板载 LED）。 */
static const uint8_t PIN_DC = 22;    /* → 模拟器 GP13 */
static const uint8_t PIN_CS = 24;    /* → 模拟器 GP14 */
static const uint8_t PIN_RESET = 26; /* → 模拟器 GP15；不接就改成 U8X8_PIN_NONE */

U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, PIN_CS, PIN_DC, PIN_RESET);

static const uint32_t BUS_HZ = 4000000;   /* 先 4 MHz；稳定后可试 8 MHz */
static const uint32_t PHASE_MS = 4000;    /* 每相位默认停留（相位可自己加长） */

/* ------------------------------------------------------------------ 打印/计数 */
static void logf(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.println(buf);
}

/* 发送计数：gCmd/gData 是本次上电累计，gPhase* 是本相位增量 */
static uint32_t gCmd = 0, gData = 0;
static uint32_t gPhaseCmd0 = 0, gPhaseData0 = 0;

/* ------------------------------------------------------------------ CRC32 */
static uint32_t crc32Update(uint32_t crc, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static uint32_t crc32Of(const uint8_t *d, size_t n)
{
    return crc32Update(0xFFFFFFFFu, d, n) ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ 原始 SPI */
static void busOn() { SPI.beginTransaction(SPISettings(BUS_HZ, MSBFIRST, SPI_MODE0)); }
static void busOff() { SPI.endTransaction(); }

/* 命令（含参数）：整段 DC=低、一次 CS */
static void sendCmds(const uint8_t *p, size_t n)
{
    digitalWrite(PIN_DC, LOW);
    digitalWrite(PIN_CS, LOW);
    for (size_t i = 0; i < n; ++i)
        SPI.transfer(p[i]);
    digitalWrite(PIN_CS, HIGH);
    gCmd += (uint32_t)n;
}

static void sendOne(uint8_t v) { sendCmds(&v, 1); }

/* 数据：DC=高、一次 CS */
static void sendData(const uint8_t *p, size_t n)
{
    digitalWrite(PIN_DC, HIGH);
    digitalWrite(PIN_CS, LOW);
    /* ⚠️ Due 上**没有**"只发不收"的块传输（见文件头第 2 条）：
     *   · SPI.transfer(void*, size_t) 是**原地**的（收到什么就覆写回缓冲区）；
     *   · SPI.transfer(tx, rx, n) 在 SAM 核里根本不存在（pico2 版用的就是它）。
     * 模拟器不驱动 MISO ⇒ 块传输会把 gImg/gImgColMajor 原地覆盖成 0x00/噪声，
     * 于是后续相位发出去的全是 0（画面变空）、本程序打印的期望 CRC 也跟着错。
     * （pico2 版现场"水平寻址被刷成空屏 + 压力相位 GDDRAM 全 0"就是这个原因。）
     * 所以这里逐字节发：SPI.transfer(b) 返回的接收字节直接丢弃。 */
    for (size_t i = 0; i < n; ++i)
        SPI.transfer(p[i]);
    digitalWrite(PIN_CS, HIGH);
    gData += (uint32_t)n;
}

/* ------------------------------------------------------------------ 参考图 */
/* 生成规则固定 ⇒ CRC 可预先算出：0xCCE79D14（宿主机测试里也有同一张图的断言） */
static uint8_t gImg[1024];        /* 页式布局：gImg[page*128 + col] */
static uint8_t gImgColMajor[1024]; /* 垂直寻址要用"列优先"的数据顺序 */

static const uint32_t REF_IMAGE_CRC = 0xCCE79D14u;

static void buildImage()
{
    for (int p = 0; p < 8; ++p) {
        for (int c = 0; c < 128; ++c) {
            uint8_t v;
            if (p == 0 || p == 7 || c == 0 || c == 127)
                v = 0xFF;                                  /* 外框（所见即所得） */
            else if ((((c >> 3) + (p >> 1)) & 1) != 0)
                v = 0x55;                                  /* 块状格纹 */
            else
                v = (uint8_t)(c * 7 + p * 31 + 0x5A);       /* 斜向渐变 */
            gImg[p * 128 + c] = v;
        }
    }
    for (int c = 0; c < 128; ++c)
        for (int p = 0; p < 8; ++p)
            gImgColMajor[c * 8 + p] = gImg[p * 128 + c];
}

/* 相位报告：gExpectCrc != 0 时与从机 gdram_crc 比较；note 说明该相位看什么 */
static uint32_t gExpectCrc = 0;
static const char *gExpectNote = "";
/* 相位可要求更长/更短的停留时间（滚动相位用；由运行框架读取） */
static uint32_t gPhaseDurationExtraMs = 0;

/* ------------------------------------------------------------------ 相位 0 */
static void phSelfCheck()
{
    const char *vec = "123456789";
    const uint32_t c1 = crc32Of((const uint8_t *)vec, 9);
    logf("  CRC32(\"123456789\") = 0x%08lX  %s", (unsigned long)c1,
        c1 == 0xCBF43926u ? "OK" : "**FAIL：主控 CRC 实现有问题，先别看后面**");

    buildImage();
    const uint32_t c2 = crc32Of(gImg, sizeof(gImg));
    logf("  参考图(1024 B)  CRC32 = 0x%08lX  %s  (期望 0x%08lX)", (unsigned long)c2,
        c2 == REF_IMAGE_CRC ? "OK" : "**FAIL**", (unsigned long)REF_IMAGE_CRC);
    logf("  判读：从机串口的 gdram_crc 与本程序 expect 行的 gdram_crc 相等即全对。");
    gExpectCrc = 0;
    gExpectNote = "本相位不发数据（只做算法自检）";
}

/* ------------------------------------------------------------------ 相位 1 */
/* 原始命令序列（不走库）：覆盖 SSD1306 常见初始化命令，验证命令通道与 DC=低 */
static const uint8_t INIT_SEQ[] = {
    0xAE,             /* display off */
    0xD5, 0x80,       /* clock divide */
    0xA8, 0x3F,       /* multiplex = 64 */
    0xD3, 0x00,       /* display offset = 0 */
    0x40,             /* start line = 0 */
    0x8D, 0x14,       /* charge pump on */
    0x20, 0x00,       /* horizontal addressing */
    0xA1,             /* segment remap */
    0xC8,             /* COM scan reversed */
    0xDA, 0x12,       /* COM pins */
    0x81, 0xCF,       /* contrast */
    0xD9, 0xF1,       /* pre-charge */
    0xDB, 0x40,       /* VCOMH */
    0xA4,             /* entire display = RAM */
    0xA6,             /* normal */
    0xAF              /* display on */
};

static void phInitSequence()
{
    sendCmds(INIT_SEQ, sizeof(INIT_SEQ));
    logf("  发出标准初始化序列 %u 字节（全部 DC=低）", (unsigned)sizeof(INIT_SEQ));
    gExpectCrc = 0;
    gExpectNote = "此时 GDDRAM 仍全 0（CRC 应为 0x00000000），disp=1、contrast=207";
}

/* ------------------------------------------------------------------ 相位 2 */
/* 页寻址整屏写入：每页 0xB0+p、列 0、128 字节 */
static void phFullFramePageMode()
{
    for (uint8_t p = 0; p < 8; ++p) {
        const uint8_t cmds[3] = { (uint8_t)(0xB0 | p), 0x00, 0x10 };
        sendCmds(cmds, 3);
        sendData(&gImg[p * 128], 128);
    }
    logf("  页寻址：8 页 × (0xB0+p,0x00,0x10 + 128 B)");
    gExpectCrc = REF_IMAGE_CRC;
    gExpectNote = "整屏参考图（页寻址）";
}

/* ------------------------------------------------------------------ 相位 3 */
/* 水平寻址整屏写入：一条 1024 B 连续数据（u8g2/Adafruit 的常用路径） */
static void phFullFrameHorizontal()
{
    const uint8_t cmds[] = { 0x20, 0x00, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
    sendCmds(cmds, sizeof(cmds));
    sendData(gImg, sizeof(gImg));
    logf("  水平寻址：0x20/0x00 + 列窗口 0..127 + 页窗口 0..7 + 1024 B");
    gExpectCrc = REF_IMAGE_CRC;
    gExpectNote = "整屏参考图（水平寻址）——应与上一相位 CRC 相同";
}

/* ------------------------------------------------------------------ 相位 4 */
/* 垂直寻址整屏写入：数据必须"列优先"排列，写完后图像与参考图相同 */
static void phFullFrameVertical()
{
    const uint8_t cmds[] = { 0x20, 0x01, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
    sendCmds(cmds, sizeof(cmds));
    sendData(gImgColMajor, sizeof(gImgColMajor));
    logf("  垂直寻址：0x20/0x01 + 同样的窗口 + 1024 B（列优先顺序）");
    gExpectCrc = REF_IMAGE_CRC;
    gExpectNote = "整屏参考图（垂直寻址）——前三种寻址模式的 CRC 必须完全一致";
}

/* ------------------------------------------------------------------ 相位 5 */
/* 局部窗口写入：只改列 32..95 / 页 2..5 的 256 字节，其余保持 */
static void phWindowWrite()
{
    const uint8_t cmds[] = { 0x20, 0x00, 0x21, 0x20, 0x5F, 0x22, 0x02, 0x05 };
    sendCmds(cmds, sizeof(cmds));

    uint8_t win[256];
    for (int i = 0; i < 256; ++i)
        win[i] = (uint8_t)(0xAA ^ (i * 3));

    /* 主控自己预测结果：参考图的该窗口被替换 */
    uint8_t expected[1024];
    memcpy(expected, gImg, sizeof(expected));
    int k = 0;
    for (int p = 2; p <= 5; ++p)
        for (int c = 0x20; c <= 0x5F; ++c)
            expected[p * 128 + c] = win[k++];
    sendData(win, sizeof(win));

    logf("  局部窗口：列 0x20..0x5F × 页 2..5 = 256 B（窗口外必须不变）");
    gExpectCrc = crc32Of(expected, sizeof(expected));
    gExpectNote = "参考图 + 窗口图案（窗口外内容不变）";
}

/* ------------------------------------------------------------------ 相位 6 */
/* 反显 / 全亮 / 显示开关：只影响渲染，不改 GDDRAM ⇒ CRC 必须不变 */
static uint32_t gCrcAfterWindow = 0;

static void phDisplayFlags()
{
    gCrcAfterWindow = gExpectCrc;

    /* ⚠️ 这 6 条命令总共只有 6 字节（4 MHz 下 ≈12 µs），而模拟器渲染有 12 ms 节流
     * （每次只画"当时的最终状态"）⇒ **背靠背发完会看不到任何变化**，这是正常的，
     * 不是模拟器坏了：0xA7→0xA5→0xAE→0xAF→0xA4→0xA6 的净效果与之前完全相同。
     * 为了能肉眼观察，这里**每一步都保持 0.8 s**（命令字与顺序完全不变，
     * 因此宿主契约测试与验收表的字节计数都不受影响）。 */
    static const struct { uint8_t cmd; const char *what; } steps[] = {
        { 0xA7, "反显 0xA7        → 画面明暗整体翻转" },
        { 0xA5, "全亮 0xA5        → 整屏点亮（忽略 GDDRAM）" },
        { 0xAE, "关显示 0xAE      → 全黑" },
        { 0xAF, "开显示 0xAF      → 恢复 GDDRAM 内容" },
        { 0xA4, "全亮关 0xA4      → 回到按 GDDRAM 显示" },
        { 0xA6, "正常显示 0xA6    → 不反显" },
    };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        sendCmds(&steps[i].cmd, 1);
        logf("  → %s（保持 1 s）", steps[i].what);
        delay(1000);
    }

    gExpectCrc = gCrcAfterWindow;
    gExpectNote = "这些命令**不改 GDDRAM**：CRC 应与上一相位完全相同；从机 disp 会依次 1/1/0/1";
    gPhaseDurationExtraMs = 1200; /* 收尾再停一下，便于核对 */
}

/* ------------------------------------------------------------------ 相位 7 */
/* 对比度扫描：0x81 的 9 档取值（影响 VFD 亮度，不影响 GDDRAM） */
static void phContrastSweep()
{
    /* 9 档：0x00 … 0xE0、0xFF。**最后一档必须是 0xFF（最亮）**：
     * 早先写成 (uint8_t)(i * 0x20)，i=8 时 0x100 截断成 0x00 ⇒ 扫描结束时把面板亮度
     * 留在 0（现场现象："亮度扫描测试后亮度停留在 0"，验收表里的 contrast=255 也就对不上）。 */
    static const uint8_t levels[9] = { 0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0xFF };
    for (int i = 0; i < 9; ++i) {
        const uint8_t cmds[2] = { 0x81, levels[i] };
        sendCmds(cmds, 2);
        /* 同理：9 档一起发（18 字节 ≈36 µs）只会看到最后一档 ⇒ 每档保持 0.4 s 才看得见亮度变化 */
        logf("  → 对比度 0x81 = 0x%02X（保持 1 s：VFD 亮度应随之变化）", levels[i]);
        delay(1000);
    }
    logf("  对比度扫描结束：最后一档 0xFF = 最亮（不会再把亮度留在 0）");
    gExpectCrc = gCrcAfterWindow;
    gExpectNote = "CRC 不变；从机 contrast= 应等于最后一次发的 0xFF";
    gPhaseDurationExtraMs = 1500;
}

/* ------------------------------------------------------------------ 相位 8 */
/* 四种硬件滚动：0x26/0x27/0x29/0x2A 设置 + 0x2F 启动 / 0x2E 停止 */
static void phScroll()
{
    static const uint8_t modes[4] = { 0x26, 0x27, 0x29, 0x2A };
    static const char *names[4] = { "0x26 右滚", "0x27 左滚", "0x29 右下滚", "0x2A 左下滚" };

    for (int i = 0; i < 4; ++i) {
        const uint8_t set[7] = { modes[i], 0x00, 0x00, 0x07, 0x07, 0x00, 0xFF };
        /* 参数：空字节, 起始页=0, 间隔=7(2 帧=16 ms), 结束页=7, 垂直偏移=0, 空字节 */
        sendCmds(set, sizeof(set));
        sendOne(0x2F);            /* 启动 */
        logf("  %s：启动，观察 1.2 s（从机 steps 应增长、gdram_crc 在变）", names[i]);
        delay(1200);
        sendOne(0x2E);            /* 停止 */
        delay(300);
    }
    logf("  四种滚动各跑 1.2 s 后已停止");
    gExpectCrc = 0;
    gPhaseDurationExtraMs = 1500;
    gExpectNote = "滚动会改写 GDDRAM ⇒ CRC 不可预测；判据是 steps>0、数字每一步都在变，停止后不再变";
}

/* ------------------------------------------------------------------ 相位 9 */
/* 压力：连续 30 帧整屏（水平寻址）不给延时，看从机会不会丢字节 */
static const int STRESS_FRAMES = 30;

static void phStress()
{
    const uint8_t cmds[] = { 0x20, 0x00, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
    const uint32_t t0 = millis();
    for (int f = 0; f < STRESS_FRAMES; ++f) {
        sendCmds(cmds, sizeof(cmds));
        sendData(gImg, sizeof(gImg));
    }
    const uint32_t us = (millis() - t0) * 1000u;
    logf("  连续 %d 帧整屏（%d B 数据）耗时 ≈ %lu ms", STRESS_FRAMES, STRESS_FRAMES * 1024,
        (unsigned long)(us / 1000u));
    gExpectCrc = REF_IMAGE_CRC;
    gExpectNote = "整屏参考图；从机 overrun/drop 必须仍为 0（否则是 CPU 取数跟不上）";
}

/* ------------------------------------------------------------------ 相位 10 */
/* u8g2 七幅诊断画面：每幅都打印 u8g2 缓冲区的 CRC，可与从机 gdram_crc 直接对照 */
static void picFrame()
{
    u8g2.drawFrame(0, 0, 128, 64);
    u8g2.drawFrame(2, 2, 124, 60);
}

static void picBasic()
{
    picFrame();
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(6, 20, "Due + u8g2");
    u8g2.drawStr(6, 36, "SSD1306 emu");
    u8g2.drawDisc(108, 26, 9);
    u8g2.drawLine(6, 44, 121, 44);
}

static void picColumnRuler()
{
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(2, 12, "column ruler");
    u8g2.drawLine(0, 16, 127, 16);
    for (int x = 0; x < 128; x += 8) {
        if (x % 32 == 0)
            u8g2.drawLine(x, 28, x, 36);
        else
            u8g2.drawPixel(x, 32);
    }
    u8g2.drawLine(0, 44, 127, 44);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(2, 52, "0    32    64    96  127");
}

static void picRowRuler()
{
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(2, 10, "row ruler");
    for (int y = 0; y < 64; y += 8) {
        if (y % 32 == 0)
            u8g2.drawLine(60, y, 68, y);
        else
            u8g2.drawPixel(64, y);
    }
    u8g2.drawLine(80, 0, 80, 63);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(84, 12, "8 rows/step");
    u8g2.drawStr(84, 26, "long=32");
}

static void picAsymmetric()
{
    u8g2.setFont(u8g2_font_10x20_tf);
    u8g2.drawStr(4, 22, "()[]<>");
    u8g2.drawStr(4, 46, "bd pq 47");
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(78, 22, "left/right");
    u8g2.drawStr(78, 38, "must be");
    u8g2.drawStr(78, 52, "readable");
}

static void picDiagonal()
{
    u8g2.drawLine(0, 0, 127, 63);
    u8g2.drawLine(127, 0, 0, 63);
    picFrame();
    for (int x = 16; x < 128; x += 32) u8g2.drawLine(x, 0, x, 63);
    for (int y = 16; y < 64; y += 16) u8g2.drawLine(0, y, 127, y);
}

static void picChecker()
{
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++)
            if (((x >> 3) + (y >> 3)) & 1)
                u8g2.drawPixel(x, y);
    u8g2.setDrawColor(0);
    u8g2.drawBox(16, 16, 96, 32);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(22, 38, "checker 8px");
}

static void picInvert()
{
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(0);
    u8g2.drawFrame(4, 4, 120, 56);
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(10, 28, "inverted");
    u8g2.drawStr(10, 46, "block");
    u8g2.setDrawColor(1);
}

static void phU8g2Pictures()
{
    typedef void (*PicFn)();
    static const PicFn fns[7] = { picBasic, picColumnRuler, picRowRuler, picAsymmetric,
        picDiagonal, picChecker, picInvert };
    static const char *names[7] = { "basic", "columns", "rows", "asym", "diagonal", "checker", "inverted" };

    logf("  七幅诊断画面，每幅 2.5 s；每幅打印 u8g2 缓冲区的 CRC32");
    for (int i = 0; i < 7; ++i) {
        u8g2.clearBuffer();
        fns[i]();
        u8g2.sendBuffer();

        /* ⚠️ 不要用 u8g2.getBufferSize()：真实的 U8g2lib.h 把它放在
         * #ifdef U8G2_USE_DYNAMIC_ALLOC 里（2.35.30 实测），默认**不存在**（会报
         * "has no member named 'getBufferSize'"）。用一定存在的 tile 尺寸自己算：
         * 128×64 全缓冲 = tile 宽 16 × 每 tile 8 字节 × tile 高 8 = 1024 B。 */
        const uint8_t *buf = u8g2.getBufferPtr();
        const uint16_t size = (uint16_t)(u8g2.getBufferTileWidth() * 8u * u8g2.getBufferTileHeight());
        const uint32_t c = crc32Of(buf, size);
        logf("    [%d/7 %-8s] %u B  frame crc=0x%08lX   ← 从机此刻应打印同一个 gdram_crc",
            i + 1, names[i], (unsigned)size, (unsigned long)c);
        delay(2500);
    }
    gExpectCrc = 0;
    gPhaseDurationExtraMs = 0;
    gExpectNote = "七幅画面各自的 CRC 已逐幅打印（留 2.5 s 供与从机对照）";
}

/* ------------------------------------------------------------------ 相位表 */
struct Phase {
    const char *name;
    void (*run)();
};

static const Phase PHASES[] = {
    { "CRC32 自检 + 判读说明", phSelfCheck },
    { "原始命令序列（初始化）", phInitSequence },
    { "整屏 · 页寻址", phFullFramePageMode },
    { "整屏 · 水平寻址", phFullFrameHorizontal },
    { "整屏 · 垂直寻址", phFullFrameVertical },
    { "局部窗口写入", phWindowWrite },
    { "反显 / 全亮 / 显示开关", phDisplayFlags },
    { "对比度扫描", phContrastSweep },
    { "四种硬件滚动", phScroll },
    { "压力：连续 30 帧整屏", phStress },
    { "u8g2 七幅诊断画面", phU8g2Pictures },
};
static const int PHASE_COUNT = (int)(sizeof(PHASES) / sizeof(PHASES[0]));

/* ------------------------------------------------------------------ 运行框架 */
static int gCur = 0;
static bool gRan = false;
static bool gPaused = false;
static uint32_t gPhaseStart = 0;

static void printHelp()
{
    Serial.println(F("  p=暂停/继续自动推进  n=下一相位  r=重跑本相位  a=从头发跑  h=帮助"));
}

static void runCurrentPhase()
{
    gPhaseCmd0 = gCmd;
    gPhaseData0 = gData;
    gExpectCrc = 0;
    gExpectNote = "";
    gPhaseDurationExtraMs = 0;

    Serial.println();
    logf("===== PHASE %d/%d : %s =====", gCur + 1, PHASE_COUNT, PHASES[gCur].name);

    PHASES[gCur].run();

    /* 自检：参考图缓冲区必须始终是我们构造的那张图。
     * 发送路径一旦把缓冲区写坏（例如误用双向 transfer(buf,n)），这里立刻报出来，
     * 免得拿着"错的期望值"去跟从机对 CRC。 */
    {
        const uint32_t c = crc32Of(gImg, sizeof(gImg));
        if (c != REF_IMAGE_CRC)
            logf("  !! 主控参考图缓冲区被破坏：crc=0x%08lX（应为 0x%08lX）⇒ 后续相位期望值不可信",
                (unsigned long)c, (unsigned long)REF_IMAGE_CRC);
    }

    const uint32_t dc = gCmd - gPhaseCmd0;
    const uint32_t dd = gData - gPhaseData0;
    logf("  sent  : cmd=+%lu data=+%lu  | 累计 cmd=%lu data=%lu",
        (unsigned long)dc, (unsigned long)dd, (unsigned long)gCmd, (unsigned long)gData);
    if (gExpectCrc != 0)
        logf("  expect: slave cmd=+%lu data=+%lu gdram_crc=0x%08lX  (%s)",
            (unsigned long)dc, (unsigned long)dd, (unsigned long)gExpectCrc, gExpectNote);
    else
        logf("  expect: slave cmd=+%lu data=+%lu gdram_crc=(不做定值比对)  (%s)",
            (unsigned long)dc, (unsigned long)dd, gExpectNote);

    gRan = true;
    gPhaseStart = millis();
}

static void handleSerial()
{
    while (Serial.available() > 0) {
        const int c = Serial.read();
        switch (c) {
        case 'p':
            gPaused = !gPaused;
            Serial.println(gPaused ? F("== PAUSED（p 继续 / n 下一相位 / r 重跑）") : F("== RUNNING"));
            break;
        case 'n':
            gCur = (gCur + 1) % PHASE_COUNT;
            gRan = false;
            break;
        case 'r':
            gRan = false;
            break;
        case 'a':
            gCur = 0;
            gRan = false;
            gPaused = false;
            Serial.println(F("== 从相位 1 重新开始"));
            break;
        case 'h':
        case '?':
            printHelp();
            break;
        default:
            break;
        }
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1500); /* 等 USB 串口枚举（UART stdio 上只是等一下） */

    Serial.println();
    Serial.println(F("=== GP1211AI SSD1306 模拟器 · 完整测试（Arduino Due 主控）==="));
    Serial.println(F("wiring: ICSP-3(76)->SCK ICSP-4(75)->MOSI D22->DC D24->CS D26->RESET GND->GND"));
    Serial.println(F("emulator: GP16 上电为高/悬空 = SSD1306 模式；本程序 4 MHz / SPI mode 0"));
    Serial.println(F("note: Due 3.3V 直连；SPI 只能从 ICSP 排针引；数据为逐字节发送"));
    printHelp();

    pinMode(PIN_DC, OUTPUT);
    pinMode(PIN_CS, OUTPUT);
    pinMode(PIN_RESET, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    digitalWrite(PIN_DC, LOW);

    u8g2.begin();          /* 顺带做一次 SSD1306 复位/初始化 */
    u8g2.setBusClock(BUS_HZ);
    buildImage();

    Serial.println(F("SPI 已就绪（4 MHz, MSB first, mode 0）"));
}

void loop()
{
    handleSerial();

    if (!gRan) {
        busOn();
        runCurrentPhase();
        busOff();
    } else if (!gPaused && millis() - gPhaseStart > (PHASE_MS + gPhaseDurationExtraMs)) {
        gCur = (gCur + 1) % PHASE_COUNT;
        gRan = false;
    }

    delay(20);
}
