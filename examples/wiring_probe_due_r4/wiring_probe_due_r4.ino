/*
 * wiring_probe_due_r4.ino —— "从机一个字节都收不到"的**分层定位探针**
 *
 * 不依赖任何显示库（不用 Adafruit_SSD1306，也不用 u8g2），只做三件事：
 *   相位 0：慢速翻转 CS/DC（0.5 Hz，10 s）—— 用从机 PINMON 判定"CS/DC 这两根线通不通"
 *   相位 1：软件位翻转（≈50 kHz）发 4 个命令字节 —— 判定"SCK/MOSI 通不通"（与 SPI 外设无关）
 *   相位 2：硬件 SPI @4 MHz 发标准初始化 + 整屏 1024 字节 —— 判定"硬件 SPI 通路 + 数据面"
 *
 * 判读（看从机串口，需要固件带 -DVFD_DEBUG_DIAG=on）：
 *   PINMON CS(GP14)=? chg=N    相位 0 后 N 必须 >0，否则 CS 没接到 GP14 / 没共地
 *   PINMON SCLK(GP11) chg=N    相位 1 后 N 必须 >0，否则 SCK 没接到 GP11
 *   rx/cmd/data 计数           相位 1 后 cmd 应 ≥ 4；相位 2 后 cmd 应 ≥ 31、data ≥ 1024
 *   gdram_crc                  相位 2 后应等于本程序打印的 buf_crc
 *
 * 支持平台（自动识别）：Arduino Due / Arduino Uno R4 Minima(及 WiFi) / RP2040
 *
 * ============================ 接线（务必按平台）============================
 *  Arduino Due（3.3 V，可直连）
 *     ICSP-3 (SCK)  → 从机 GP11        ⚠️ Due 的 SPI 只在 ICSP 2×3 排针上，
 *     ICSP-4 (MOSI) → 从机 GP12           数字口 11/12/13 不是 SPI（那是 Uno 的排法）
 *     D6 (DC)       → 从机 GP13        ⚠️ 从机要求 DC 紧跟在 MOSI 后一个 GPIO
 *     D7 (CS)       → 从机 GP14
 *     D8 (RESET)    → 从机 GP15（可省）
 *     GND           → GND
 *
 *  Arduino Uno R4 Minima（5 V 逻辑）
 *     D13 (SCK)     → 从机 GP11        ⚠️ 5 V 直连 RP2040 超规格（输入钳位二极管会导通），
 *     D11 (COPI)    → 从机 GP12           建议串 1 kΩ 或用电平转换模块；模块要两路供电
 *     D6  (DC)      → 从机 GP13
 *     D7  (CS)      → 从机 GP14
 *     D8  (RESET)   → 从机 GP15（可省）
 *     GND           → GND（必须共地）
 *
 * 三条最容易踩的坑：
 *   1. DC/CS 不要选在主机 SPI 外设自己的脚上。例：Uno R4 上 D11/D12/D13 是 SPI 脚，
 *      若把 DC 或 CS 设成 11/12，SPI.begin() 会把这两个脚收归外设 => CS 永远拉不低 => 0 字节。
 *   2. while (!Serial) 在 Due / R4 上会一直卡住（它们的 Serial 是 USB CDC，要等主机打开串口），
 *      于是根本执行不到发送代码。本程序不使用 while (!Serial)。
 *   3. 从机侧 GP16 上电必须为高/悬空（SSD1306 模式）；为低会进入测试图像模式，永远收不到字节。
 */

#include <Arduino.h>
#include <SPI.h>

/* ------------------------------------------------------------------ 平台识别 */
#if defined(ARDUINO_ARCH_SAM)
  #define PROBE_BOARD "Arduino Due (SAM3X, 3.3V)"
  #define PROBE_SPI_NOTE "硬SPI 只在 ICSP 排针：ICSP-3=SCK / ICSP-4=MOSI；D11/12/13 不是 SPI"
#elif defined(ARDUINO_ARCH_RENESAS) || defined(ARDUINO_UNOR4_MINIMA) || defined(ARDUINO_UNOR4_WIFI)
  #define PROBE_BOARD "Arduino Uno R4 (RA4M1, 5V 逻辑)"
  #define PROBE_SPI_NOTE "硬SPI: D13=SCK / D11=COPI / D12=CIPO；5V 直连 RP2040 超规格"
#elif defined(ARDUINO_ARCH_RP2040)
  #define PROBE_BOARD "RP2040/RP2350 (arduino-pico, 3.3V)"
  #define PROBE_SPI_NOTE "硬SPI: GP18=SCK / GP19=TX"
#else
  #define PROBE_BOARD "未知 Arduino 平台"
  #define PROBE_SPI_NOTE "请自查该板 SPI 引脚位置"
#endif

/* DC/CS/RESET 用普通 GPIO；不要与 SPI 外设脚重合 */
#ifndef PROBE_PIN_DC
#define PROBE_PIN_DC   6
#endif
#ifndef PROBE_PIN_CS
#define PROBE_PIN_CS   7
#endif
#ifndef PROBE_PIN_RST
#define PROBE_PIN_RST  8
#endif

/* 位翻转（相位 1）用的两根脚：**不能**依赖 SCK/MOSI 宏（各核心命名不同）。
 * 这里按平台给默认值，它们必须与硬件 SPI 实际使用的两根脚一致。 */
#ifndef PROBE_BB_SCK
  #if defined(ARDUINO_ARCH_SAM)
    #define PROBE_BB_SCK  76    /* Due: ICSP-3 = GPIO76 */
  #elif defined(ARDUINO_ARCH_RENESAS) || defined(ARDUINO_UNOR4_MINIMA) || defined(ARDUINO_UNOR4_WIFI)
    #define PROBE_BB_SCK  13    /* Uno R4: D13 = SCK */
  #else
    #define PROBE_BB_SCK  18    /* Pico: GP18 */
  #endif
#endif
#ifndef PROBE_BB_MOSI
  #if defined(ARDUINO_ARCH_SAM)
    #define PROBE_BB_MOSI 75    /* Due: ICSP-4 = GPIO75 */
  #elif defined(ARDUINO_ARCH_RENESAS) || defined(ARDUINO_UNOR4_MINIMA) || defined(ARDUINO_UNOR4_WIFI)
    #define PROBE_BB_MOSI 11    /* Uno R4: D11 = COPI */
  #else
    #define PROBE_BB_MOSI 19    /* Pico: GP19 */
  #endif
#endif

static const uint32_t PROBE_BUS_HZ = 4000000UL;

/* ------------------------------------------------------------------ 工具 */
static uint32_t crc32Buf(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

static uint8_t patternByte(int i)
{
    return (uint8_t)((i * 37 + (i >> 3) * 11 + 0x5A) & 0xFF);
}

/* ---------------- 位翻转发送（不碰 SPI 外设，约 50 kHz）----------------
 * 模式 0：数据先就位，SCK 上升沿采样。DC/CS 由调用者控制。 */
static void bbWriteByte(uint8_t b)
{
    for (int i = 7; i >= 0; --i) {
        digitalWrite(PROBE_BB_MOSI, (b >> i) & 1u);
        delayMicroseconds(5);
        digitalWrite(PROBE_BB_SCK, HIGH);
        delayMicroseconds(5);
        digitalWrite(PROBE_BB_SCK, LOW);
    }
}

static void bbSelect(bool on)
{
    digitalWrite(PROBE_PIN_CS, on ? LOW : HIGH);
    delayMicroseconds(5);
}

static void bbCmd(uint8_t c)
{
    digitalWrite(PROBE_PIN_DC, LOW);
    bbWriteByte(c);
}

/* 相位 1：4 个命令字节（0xAE 显示关 / 0xAF 显示开 / 0x81 0x7F 对比度） */
static void phase1_bitbang()
{
    Serial.println(F("[相位 1] 位翻转（约 50 kHz）发 4 个命令字节：AE AF 81 7F"));
    bbSelect(true);
    bbCmd(0xAE);
    bbCmd(0xAF);
    bbCmd(0x81);
    bbCmd(0x7F);
    bbSelect(false);
    Serial.println(F("         期望：从机 cmd 增加 4、data 不增加、unknown 不增长"));
}

/* 相位 2：硬件 SPI @4 MHz，标准初始化 + 整屏 1024 字节 */
static void phase2_hardspi()
{
    static const uint8_t init[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock div */
        0xA8, 0x3F,       /* multiplex 64 */
        0xD3, 0x00,       /* display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on */
        0x20, 0x00,       /* horizontal addressing */
        0xA1, 0xC8,       /* seg remap + com scan dec（与主机缓冲同向） */
        0xDA, 0x12,       /* com pins */
        0x81, 0xCF,       /* contrast */
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4, 0xA6, 0x2E, /* entire off / normal / scroll off */
        0xAF              /* display on */
    };
    static const uint8_t frameCmds[] = { 0x22, 0x00, 0xFF, 0x21, 0x00, 0x7F };

    Serial.println(F("[相位 2] 硬件 SPI @4 MHz：25 字节初始化 + 6 字节窗口命令 + 1024 字节数据"));
    digitalWrite(PROBE_PIN_DC, LOW);
    digitalWrite(PROBE_PIN_CS, LOW);
    for (size_t i = 0; i < sizeof(init); ++i)
        SPI.transfer(init[i]);
    for (size_t i = 0; i < sizeof(frameCmds); ++i)
        SPI.transfer(frameCmds[i]);
    digitalWrite(PROBE_PIN_DC, HIGH);          /* 数据相 */

    uint8_t buf[1024];
    for (int i = 0; i < 1024; ++i)
        buf[i] = patternByte(i);
    for (int i = 0; i < 1024; ++i)
        SPI.transfer(buf[i]);                  /* 逐字节：Due 的块传输会原地覆盖 */

    digitalWrite(PROBE_PIN_CS, HIGH);

    Serial.print(F("         期望：从机 cmd 增加 31、data 增加 1024，gdram_crc=0x"));
    Serial.println(crc32Buf(buf, 1024), HEX);
}

/* 相位 0：慢速翻 CS/DC，让从机 PINMON 能看见这两根线 */
static void phase0_pinwiggle()
{
    Serial.println(F("[相位 0] CS/DC 慢速翻转 10 s（用从机 PINMON 看 chg 是否增长）"));
    for (int i = 0; i < 20; ++i) {
        digitalWrite(PROBE_PIN_CS, LOW);
        digitalWrite(PROBE_PIN_DC, HIGH);
        delay(250);
        digitalWrite(PROBE_PIN_CS, HIGH);
        digitalWrite(PROBE_PIN_DC, LOW);
        delay(250);
    }
    Serial.println(F("         期望：PINMON 里 CS(GP14) 与 DC(GP13) 的 chg 至少 20"));
}

void setup()
{
    Serial.begin(115200);
    /* 从机（RP2040）启动的几十毫秒收不到任何字节 —— 等够再发 */
    delay(1500);

    Serial.println();
    Serial.println(F("=== SSD1306 模拟器接线探针 ==="));
    Serial.print(F("平台 : ")); Serial.println(F(PROBE_BOARD));
    Serial.print(F("SPI  : ")); Serial.println(F(PROBE_SPI_NOTE));
    Serial.print(F("DC=")); Serial.print(PROBE_PIN_DC);
    Serial.print(F("  CS=")); Serial.print(PROBE_PIN_CS);
    Serial.print(F("  RESET=")); Serial.println(PROBE_PIN_RST);

    /* DC/CS/RESET 必须是普通 GPIO；若落在 SPI 外设脚上，SPI.begin() 会抢走它们 */
    pinMode(PROBE_PIN_DC, OUTPUT);
    pinMode(PROBE_PIN_CS, OUTPUT);
    pinMode(PROBE_PIN_RST, OUTPUT);
    digitalWrite(PROBE_PIN_CS, HIGH);
    digitalWrite(PROBE_PIN_DC, LOW);

    digitalWrite(PROBE_PIN_RST, HIGH);     /* 拉一次 RESET（约 10 ms 低） */
    delay(1);
    digitalWrite(PROBE_PIN_RST, LOW);
    delay(10);
    digitalWrite(PROBE_PIN_RST, HIGH);

    /* 位翻转用到的两根线（Uno R4 上 D13/D11 就是 SPI 脚，位翻转阶段先当普通 GPIO，
     * 等下面 SPI.begin() 时再交回外设） */
    pinMode(PROBE_BB_SCK, OUTPUT);
    pinMode(PROBE_BB_MOSI, OUTPUT);
    digitalWrite(PROBE_BB_SCK, LOW);
    digitalWrite(PROBE_BB_MOSI, LOW);

    phase0_pinwiggle();
    phase1_bitbang();

    SPI.begin();
    SPI.beginTransaction(SPISettings(PROBE_BUS_HZ, MSBFIRST, SPI_MODE0));
    phase2_hardspi();
    SPI.endTransaction();

    Serial.println();
    Serial.println(F("=== 探针结束 ==="));
    Serial.println(F("若相位 1/2 的 cmd/data 仍为 0：先按相位 0/1 读 PINMON，再核对接线表；"));
    Serial.println(F("若相位 1 通、相位 2 不通：硬件 SPI 的 SCK/MOSI 不在你接的那两个脚上（Due 最常见）。"));
}

void loop()
{
    delay(1000);
}
