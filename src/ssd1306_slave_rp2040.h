/*
 * ssd1306_slave_rp2040 —— SSD1306 4 线 SPI 从机传输层（RP2040）。
 *
 * 数据通路：
 *     SCLK/MOSI/DC/CS（4 线）→ PIO 状态机（逐位采样，每字节组成 9 位字）
 *        → PIO RX FIFO → DMA（32bit，环形写地址，无需 CPU 参与）
 *        → 环形缓冲 → 应用逐字节取出（value + dc）交给 Ssd1306Emulator
 *
 * 为什么用 PIO 而不用硬件 SPI 从机：硬件 SPI 只能给出数据字节，拿不到"每个字节时刻的
 * DC 电平"（SSD1306 的命令/数据区分就靠 DC）。PIO 在每个字节的最后一位同时采 DC，
 * 组成 9 位字，从机侧天然获得逐字节 DC；而且不关心 CS 是否在整段通信中一直保持低
 * （u8g2 这类库会在一次 CS 内切换 DC），兼容性更好。
 *
 * 资源占用：1 个 PIO 状态机（15 条指令，任意 PIO 块）+ 1 个 DMA 通道 + **16 KB** 环形缓冲。
 * 注意：若扫描引擎选了 pio，两者必须使用**不同的 PIO 块**（vfd_scan 程序已占满 32 条指令）。
 */
#ifndef VFD_SSD1306_SLAVE_RP2040_H
#define VFD_SSD1306_SLAVE_RP2040_H

#include "pico/stdlib.h"

#include "hardware/dma.h"
#include "hardware/pio.h"

namespace vfd {

/* 环形缓冲：4096 个 32bit 字 = 16 KB（必须是 2 的幂，DMA 环形回绕用）。
 *
 * 尺寸有硬要求：SSD1306 主机**一次 CS 内连写整屏**（水平寻址 = 1024 B 数据 + 若干命令）
 * 时突发长度就超过 1 KB，而 CPU 还要同时干活（渲染、串口打印）。旧值 256 字（1 KB）在
 * CPU 一次轮询/打印之间就会被 DMA 跑满好几圈，配合"写地址 mod N 差分"的计数方式，
 * 整圈的字会被**静默丢弃**（现场：1024 B 只收到 256 B，部分 GDDRAM 不更新）。
 * 现在 16 KB = 4096 字，而一屏（水平寻址整屏）= 1024 字 ⇒ 能**整装 4 屏**；
 * 计数也改成了精确的"基数 + 连续搬运差分"（见 ssd1306_pio_wire.h 的 ringWrittenFromBase）。 */
constexpr uint32_t SSD1306_SLAVE_RING_WORDS = 4096;

/* ---- 默认接线（可在 CMake 里用 -DVFD_EMU_PIN_xxx=n 覆盖）---- */
#ifndef VFD_EMU_PIN_SCK
#define VFD_EMU_PIN_SCK 11
#endif
#ifndef VFD_EMU_PIN_MOSI /* PIO 的 in_base；DC 必须接在它 +1 */
#define VFD_EMU_PIN_MOSI 12
#endif
#ifndef VFD_EMU_PIN_DC
#define VFD_EMU_PIN_DC 13
#endif
#ifndef VFD_EMU_PIN_CS
#define VFD_EMU_PIN_CS 14
#endif
#ifndef VFD_EMU_PIN_RESET /* 0xFF = 不接 RESET */
#define VFD_EMU_PIN_RESET 15
#endif
#ifndef VFD_EMU_PIO_INST /* 0 = pio0, 1 = pio1；必须与扫描引擎的 PIO 块不同 */
#define VFD_EMU_PIO_INST 1
#endif
#ifndef VFD_EMU_PIO_SM
#define VFD_EMU_PIO_SM 0
#endif
#ifndef VFD_EMU_DMA_CH /* 勿与扫描引擎冲突（tick 用 0/1，pio 用 0） */
#define VFD_EMU_DMA_CH 2
#endif

struct Ssd1306SpiSlaveConfig {
    uint8_t pin_sck;   /* 主机 SCLK */
    uint8_t pin_mosi;  /* 主机 MOSI（PIO 的 in_base） */
    uint8_t pin_dc;    /* 命令/数据选择（**必须 = pin_mosi + 1**，PIO 用 in pins,2 一起采） */
    uint8_t pin_cs;    /* 片选（PIO 的 jmp_pin，低有效） */
    uint8_t pin_reset; /* SSD1306 RESET，可选：设为 0xFF 表示不接 */
    PIO pio;           /* 建议与扫描引擎使用不同的 PIO 块 */
    uint8_t sm;        /* 状态机编号 0..3 */
    int8_t dma_ch;     /* DMA 通道（勿与扫描引擎冲突） */
};

Ssd1306SpiSlaveConfig defaultSsd1306SpiSlaveConfig();

class Ssd1306SpiSlave {
public:
    explicit Ssd1306SpiSlave(const Ssd1306SpiSlaveConfig &cfg = defaultSsd1306SpiSlaveConfig());
    ~Ssd1306SpiSlave();

    /* 引脚/PIO/DMA 初始化并开始接收；返回 false 时 lastError() 给出原因 */
    bool begin();

    /* 取一个已接收字节；dc=false 表示该字节是命令（DC 低）。没有数据时返回 false。 */
    bool popByte(uint8_t &value, bool &dc);

    /* 主循环周期调用：轮询 RESET 引脚、补充 DMA 计数、检测环形缓冲溢出 */
    void task();

    /* ---- 诊断 ---- */
    const char *lastError() const { return _error; }
    bool running() const { return _running; }
    uint32_t receivedCount() const { return _received; }

    /* ---- 只读诊断访问器（接线/卡点排查用，不影响数据通路）---------------
     * 判读：
     *   pc 停在 +0（wait_cs）      ⇒ 状态机根本没被 CS 触发（引脚补丁/极性/程序起点问题）
     *   pc 在 +2..+6（bit_loop 内）且 rx_fifo=0 ⇒ 时钟进来了但没走到 push
     *   rx_fifo > 0 而 rx(=receivedCount) 一直 0 ⇒ PIO 在正常产出，卡在 DMA 搬运
     *   dma_wr 不变 ⇒ DMA 没动（DREQ/通道/地址配置问题） */
    int  debugPcOffset() const;      /* 相对本程序起点的 PC（-1 = 不可用） */
    const char *debugPcName() const; /* PC 对应指令名（便于直接判读） */
    int  debugRxFifo() const;
    uint32_t debugDmaWriteAddr() const;
    uint32_t debugDmaRemaining() const;
    int  debugDmaBusy() const;
    /* 装载镜像（**不是**从 INSTR_MEM 回读）：RP2040 的 PIO INSTR_MEM 是只写（WO）的，
     * 读回恒为 0（RP2040.svd：PIO_INSTR_MEM0_ACCESS "WO" —— 旧版的 instr[] 全 0 就是这个原因），
     * 所以这里保存的是我们真正写进指令内存、并已通过引脚自检的那份字。
     * 用它 + debugInBase() 就能解出"SM 到底在等哪个脚"（见 ssd1306_pio_wire.h decodeWait）。 */
    uint16_t debugLoadedWord(int i) const;  /* 第 i 条（相对程序起点）；越界返回 0 */
    uint32_t debugLoadedWords() const;      /* 已装载的指令条数 */
    /* 硬件寄存器字段 */
    uint32_t debugPinCtrl() const;          /* PINCTRL：IN_BASE(19:15) OUT_BASE SET_BASE SIDESET_BASE */
    uint8_t  debugInBase() const;           /* PINCTRL.IN_BASE：in pins / wait pin 的基址 */
    uint8_t  debugJmpPin() const;           /* EXECCTRL.JMP_PIN：jmp pin 的绝对 GPIO */
    uint32_t debugExecCtrl() const;         /* 内含 JMP_PIN 字段 */
    uint32_t debugShiftCtrl() const;        /* 内含 IN_SHIFTDIR / AUTOPUSH / FJOIN 等 */
    bool debugTakeIrqFlag();                /* 读并清除 PIO IRQ0 标志（push 计数用） */
    uint32_t droppedCount() const { return _dropped; }
    uint32_t overrunCount() const { return _overruns; }
    uint32_t resetEvents() const { return _resetEvents; }
    bool resetAsserted() const { return _resetAsserted; }
    /* RESET 引脚由低变高的那次事件（应用应据此复位模拟器状态） */
    bool consumeResetEvent();

    /* ---- 事务间隔直方图（**只观测、零行为影响**）----
     * 目的：为 CS 边界重同步的去抖窗口提供**实测依据**。必须分清两类间隔：
     *   · 同一逻辑操作内背靠背的事务（u8g2 整帧 sendBuffer 时每个 tile 一个事务）；
     *   · 两次独立操作之间（例如 init 序列与随后的第一帧之间）。
     * 去抖窗口必须**大于前者、小于后者**，否则会把一条逻辑操作切碎、误丢参数。
     * 桶（µs）见 txnGapBucketLabel()。 */
    static constexpr int TXN_GAP_BUCKETS = 12;
    uint32_t txnCount() const { return _txnCount; }
    uint32_t txnGapBucket(int i) const
    {
        return (i >= 0 && i < TXN_GAP_BUCKETS) ? _txnGap[i] : 0u;
    }
    static const char *txnGapBucketLabel(int i);

    /* ---- 事务边界（CS 释放 + 空闲去抖）----
     * PIO 侧知道事务边界却没交给模拟器；而一条命令是"命令字节 + 若干参数字节"的多字节序列，
     * 少一个字节就会把下一条命令吃成参数、且**永不自行纠正** —— 现场症状正是"unknown 持续
     * 增长、gdram_crc 永不匹配、只能 RESET 恢复"。
     * 这里用**纯 CPU 侧**办法补回边界（不动时序关键的 PIO 程序）：
     *   · CS 抬升并空闲满 CS_IDLE_DEBOUNCE_US ⇒ 置一次事务结束标志；
     *   · 应用在**把环形缓冲排空之后**调 consumeTransactionEnd()；
     *   · 模拟器据此作废"没来得及收齐参数"的那条命令，下一事务重新对齐。
     * ⚠️ 调用约定：必须在 popByte() 返回 false（排空）之后调用。 */
    bool consumeTransactionEnd();
    uint32_t transactionEndCount() const { return _txnEnds; }
    uint32_t pendingWords() const { return static_cast<uint32_t>(_pending < 0 ? 0 : _pending); }
    /* DMA 已搬满的整圈数（累计写入量 = laps*N + (N - remaining)，诊断用） */
    uint32_t lapCount() const { return _laps; }
    /* PIO 是否因为 RX FIFO 满而卡住（DMA 跟不上时会置位，正常应为 false）。
     * ⚠️ FDEBUG 的 RXSTALL 是**粘性**位：本函数**读取即清除**，所以它回答的是
     *    "自上次询问以来是否卡过"，而不是"此刻是否卡着"。正因如此它不能是 const。 */
    bool stalled();

private:
    void updatePending();
    void resetStream();
    void patchProgramPins(uint16_t *words, uint length);
    /* 装载后自检：程序里必须全是 wait gpio，且引脚与数量都符合约定 */
    bool verifyWaitPins(const uint16_t *words, uint length);

    Ssd1306SpiSlaveConfig _cfg;
    int _offset;
    /* 装载镜像：我们写进 PIO 指令内存的那份字（RP2040 无法回读 INSTR_MEM） */
    uint16_t _words[32];
    uint _wordCount;
    dma_channel_config _dmaConfig;
    /* 环形缓冲：**16 KB 对齐的静态成员**（定义在 .cpp）。
     * DMA 的写地址回绕要求基址按环大小对齐；若做成普通成员，类的对齐要求会把对象的
     * sizeof 一并撑到 48 KB（16 KB 环 + 32 KB 填充）——实测 .bss 因此多花 32 KB。
     * 本工程只用一路从机，静态存储语义完全相同。 */
    static uint32_t _ring[SSD1306_SLAVE_RING_WORDS]; /* 16 KB 对齐写在 .cpp 的定义处 */
    uint32_t _popped; /* 累计已取走的字数；缓冲内位置 = _popped & (N-1) */
    uint32_t _base;   /* 补计数前的累计写入量（精确计数：_base + (DMA_COUNT_FULL - remaining)） */
    uint32_t _laps;   /* 诊断用：累计搬了多少整圈（在 updatePending 里算出） */
    int32_t _pending; /* 已写入但未取走 */
    uint32_t _received;
    uint32_t _dropped;
    uint32_t _overruns;
    uint32_t _resetEvents;
    bool _resetAsserted;
    bool _pendingResetEvent;
    /* 事务观测（诊断，不参与任何判定） */
    uint32_t _txnCount;
    uint32_t _txnGap[TXN_GAP_BUCKETS];
    uint32_t _csIdleSinceUs;
    bool _csIdleLast;
    bool _txnSeen;
    uint32_t _txnEnds;
    bool _pendingTxnEnd;
    bool _running;
    const char *_error;
};

} /* namespace vfd */

#endif /* VFD_SSD1306_SLAVE_RP2040_H */
