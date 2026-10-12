/*
 * pico2_adafruit_test.ino —— 用 Raspberry Pi Pico 2（RP2350，arduino-pico 核心）+
 *                            Adafruit_SSD1306 测试本工程的"SSD1306 行为模拟"
 *
 * 与 examples/pico2_u8g2_test 的区别：那个用 u8g2（4 MHz），这个用 Adafruit_SSD1306，
 * 并且**可以在运行时切换 SPI 时钟（4 / 8 / 10 MHz）**，用来判定"异常是否由默认 8 MHz 引起"。
 *
 * 环境
 *   Arduino IDE + Earle Philhower 的 "Raspberry Pi Pico/RP2040" 核心，开发板选 "Raspberry Pi Pico 2"
 *   库管理器装 "Adafruit SSD1306"（会一并装上 Adafruit GFX Library）
 *
 * 接线（主控 Pico 2  →  模拟器 RP2040）
 *   GP18 (SPI0 SCK)  →  GP11  SCLK
 *   GP19 (SPI0 TX)   →  GP12  MOSI
 *   GP14 (普通 GPIO) →  GP13  DC     ⚠️ 必须是模拟器的 GP13（= 它的 MOSI+1）
 *   GP15 (普通 GPIO) →  GP14  CS
 *   GP20 (普通 GPIO) →  GP15  RESET  （可省：把 PIN_RESET 改成 -1 即可）
 *   GND              →  GND          （必须共地）
 *
 *   模拟器侧：GP16 上电保持高/悬空 ⇒ SSD1306 模拟模式（串口打印 "mode: SSD1306 emulator"）
 *
 * 串口（115200）交互：n 下一相位 · b 切换 SPI 时钟 · s 连续压测 20 s · h 帮助
 *
 * 判读（两端对照，不需要相机）
 *   本程序每个相位打印自己 1024 B 缓冲的 CRC32；与模拟器每秒那行的 gdram_crc 对照：
 *       ... gdram_crc=0xZZZZZZZZ
 *   一致 ⇒ 整帧每一个字节都落对了位置；不一致 ⇒ 从机位相位/寻址状态有问题。
 *   模拟器那行里同时有 stallfix / overrun / drop / resync / unknown，正常应全为 0。
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/* ------------------------------------------------------------------ 接线/参数 */
static const int8_t PIN_DC    = 14;   /* → 模拟器 GP13 */
static const int8_t PIN_CS    = 15;   /* → 模拟器 GP14 */
static const int8_t PIN_RESET = 20;   /* → 模拟器 GP15；不接就改成 -1 */

/* ⚠️ Adafruit_SSD1306 的 bitrate 是构造函数最后一个参数，库默认值是 8000000（8 MHz）。
 *    本工程所有实机验证过的示例都是 4 MHz；先用 4 MHz 确认功能，再按 'b' 切到 8 MHz 对比。 */
static uint32_t gBusHz = 4000000;

/* 允许运行时改 bitrate：spiSettings 是 protected 成员，子类里改 */
class SSD1306Var : public Adafruit_SSD1306 {
public:
    SSD1306Var(uint16_t w, uint16_t h, SPIClass *spi, int8_t dc, int8_t rst, int8_t cs, uint32_t hz)
        : Adafruit_SSD1306(w, h, spi, dc, rst, cs, hz) {}
    void setBusHz(uint32_t hz) { spiSettings = SPISettings(hz, MSBFIRST, SPI_MODE0); }
};

static SSD1306Var oled(128, 64, &SPI, PIN_DC, PIN_RESET, PIN_CS, gBusHz);

/* ------------------------------------------------------------------ CRC32 */
/* 与从机 src/vfd_crc32.h 完全同款（IEEE 802.3，poly 0xEDB88320，初值/异或全 1） */
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

static void crcSelfTest()
{
    const char *s = "123456789";
    const uint32_t c = crc32Buf((const uint8_t *)s, 9);
    Serial.print(F("CRC32 self-test: 0x"));
    Serial.print(c, HEX);
    Serial.println(c == 0xCBF43926UL ? F("  OK") : F("  *** FAIL ***"));
}

/* ------------------------------------------------------------------ 相位画面 */
/* 前 6 幅与 examples/pico2_u8g2_test 的判读图一一对应，方便两个库并排比较 */
static void clearAll() { oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1); }

static void screenBasic()
{
    clearAll();
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    oled.println(F("VFD GP1211AI"));
    oled.setTextSize(1);
    oled.setCursor(0, 20);
    oled.println(F("Adafruit_SSD1306"));
    oled.setCursor(0, 32);
    oled.print(F("SPI "));
    oled.print(gBusHz / 1000000);
    oled.println(F(" MHz"));
    oled.setCursor(0, 48);
    oled.println(F("1234567890"));
}

static void screenColumnRuler()
{
    clearAll();
    for (int x = 0; x < 128; x += 8) {
        oled.drawFastVLine(x, 0, 64, SSD1306_WHITE);
        if (x % 32 == 0) oled.drawFastVLine(x + 4, 0, 40, SSD1306_WHITE);
    }
    oled.setCursor(0, 0);   oled.print(F("0"));
    oled.setCursor(56, 0);  oled.print(F("64"));
    oled.setCursor(112, 0); oled.print(F("127"));
}

static void screenRowRuler()
{
    clearAll();
    for (int y = 0; y < 64; y += 4)
        oled.drawFastHLine(0, y, 128, SSD1306_WHITE);
    for (int y = 0; y < 64; y += 16) {
        oled.setCursor(0, y);
        oled.print(y);
    }
}

static void screenAsymmetric()
{
    clearAll();
    oled.fillRect(0, 0, 24, 12, SSD1306_WHITE);
    oled.drawRect(104, 52, 24, 12, SSD1306_WHITE);
    for (int i = 0; i < 24; ++i)
        oled.drawPixel(104 + i, i, SSD1306_WHITE);
    oled.setCursor(4, 40);
    oled.print(F("L/R  U/D"));
}

static void screenDiagonal()
{
    clearAll();
    for (int i = 0; i < 64; ++i) {
        oled.drawPixel(i * 2, i, SSD1306_WHITE);
        oled.drawPixel(127 - i * 2, i, SSD1306_WHITE);
    }
}

static void screenChecker()
{
    clearAll();
    for (int y = 0; y < 64; y += 8)
        for (int x = 0; x < 128; x += 8)
            if (((x / 8) + (y / 8)) & 1)
                oled.fillRect(x, y, 8, 8, SSD1306_WHITE);
}

static void screenFullOn()
{
    clearAll();
    oled.fillScreen(SSD1306_WHITE);
}

static void screenInvert()
{
    clearAll();
    oled.fillRect(0, 0, 128, 64, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setTextSize(2);
    oled.setCursor(8, 24);
    oled.print(F("INVERT"));
    oled.setTextColor(SSD1306_WHITE);
}

typedef void (*ScreenFn)();
static const ScreenFn kScreens[] = {
    screenBasic, screenColumnRuler, screenRowRuler,
    screenAsymmetric, screenDiagonal, screenChecker, screenFullOn, screenInvert,
};
static const char *kScreenNames[] = {
    "basic", "cols", "rows", "asym", "diag", "checker", "full-on", "invert",
};
static const int kScreenCount = (int)(sizeof(kScreens) / sizeof(kScreens[0]));
static int gScreen = 0;

/* ------------------------------------------------------------------ 发一帧 + 两端对照 */
static void sendFrame(const char *tag)
{
    oled.display();
    const uint32_t crc = crc32Buf(oled.getBuffer(), 1024);
    Serial.print(F("phase "));
    Serial.print(gScreen);
    Serial.print(F(" ("));
    Serial.print(kScreenNames[gScreen]);
    Serial.print(F(") @"));
    Serial.print(gBusHz / 1000000);
    Serial.print(F(" MHz  buf_crc=0x"));
    Serial.print(crc, HEX);
    Serial.print(F("   ["));
    Serial.print(tag);
    Serial.println(F("]  <- 与从机 gdram_crc 对照"));
}

static void showScreen(int idx, const char *tag)
{
    gScreen = ((idx % kScreenCount) + kScreenCount) % kScreenCount;
    kScreens[gScreen]();
    sendFrame(tag);
}

/* ------------------------------------------------------------------ 压测 */
static void stress(uint32_t seconds)
{
    Serial.print(F("stress: "));
    Serial.print(seconds);
    Serial.println(F(" s 连续 display()（观察从机 stallfix/overrun/drop/unknown 是否保持 0）"));
    const uint32_t t0 = millis();
    uint32_t frames = 0;
    while (millis() - t0 < seconds * 1000UL) {
        kScreens[gScreen]();
        oled.display();
        frames++;
        if ((frames & 0x3F) == 0 && Serial.available())
            break;
    }
    Serial.print(F("stress done: "));
    Serial.print(frames);
    Serial.println(F(" frames"));
    sendFrame("after stress");
}

static void help()
{
    Serial.println(F("命令：n=下一相位  b=切换 SPI 时钟(4/8/10 MHz)  s=压测 20 s  h=帮助"));
}

void setup()
{
    Serial.begin(115200);
    /* ⚠️ 上电后必须等一会儿再发初始化：从机（RP2040）自己启动的几十毫秒收不到任何字节。
     *    本仓库 u8g2 示例同样有 delay(1500)。 */
    delay(1500);

    Serial.println();
    Serial.println(F("=== Pico 2 -> GP1211AI SSD1306 emulator (Adafruit_SSD1306) ==="));
    Serial.println(F("wiring: GP18->SCK GP19->MOSI GP14->DC GP15->CS GP20->RESET GND->GND"));
    Serial.println(F("emulator side: GP16 must be HIGH at power-up (SSD1306 mode)"));
    crcSelfTest();

    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0)) {
        Serial.println(F("*** SSD1306 allocation failed ***"));
        for (;;) delay(1000);
    }
    oled.setBusHz(gBusHz);
    Serial.print(F("Adafruit_SSD1306 ready, SPI mode 0 @"));
    Serial.print(gBusHz / 1000000);
    Serial.println(F(" MHz"));
    help();

    showScreen(0, "first frame");
}

void loop()
{
    static uint32_t last = 0;
    if (millis() - last >= 2000) {
        last = millis();
        showScreen(gScreen + 1, "auto");
    }

    if (!Serial.available())
        return;
    const int c = Serial.read();
    switch (c) {
    case 'n':
        last = millis();
        showScreen(gScreen + 1, "manual");
        break;
    case 'b': {
        gBusHz = (gBusHz == 4000000UL) ? 8000000UL : (gBusHz == 8000000UL ? 10000000UL : 4000000UL);
        oled.setBusHz(gBusHz);
        Serial.print(F("SPI clock -> "));
        Serial.print(gBusHz / 1000000);
        Serial.println(F(" MHz（对比各时钟下从机的 stallfix/unknown/gdram_crc）"));
        last = millis();
        showScreen(gScreen, "clock switch");
        break;
    }
    case 's':
        stress(20);
        last = millis();
        break;
    case 'h':
    default:
        help();
        break;
    }
}
