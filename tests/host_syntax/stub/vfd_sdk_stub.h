/*
 * Pico SDK API 形状桩 —— 仅用于宿主机语法/类型检查（不是 SDK 本体）。
 *
 * 覆盖本工程用到的三类接口：
 *   1. hardware_spi / hardware_dma / hardware_pwm / hardware_timer / hardware_gpio /
 *      hardware_clocks / pico_stdlib / pico_time / pico_platform
 *   2. hardware_pio（PIO 引擎）
 *   3. vfd_scan.pio.h 生成头的等价声明（见同目录 vfd_scan.pio.h）
 *
 * 注意：函数名/签名已按 pico-sdk 2.1.0 的真实头文件校对，但仍可能滞后；
 * 最终以真实 SDK 构建为准（见 README 的构建验证记录）。
 */
#ifndef VFD_SDK_STUB_H
#define VFD_SDK_STUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Pico SDK 在 pico/types.h 里提供 uint 别名（C++ 下同样可用） */
typedef unsigned int uint;

/* ------------------------------------------------------------ hardware/spi */
typedef struct {
    volatile uint32_t cr0, cr1, dr, sr, cpsr, imsc, ris, mis, icr, dmacr;
} spi_hw_t;

typedef struct spi_inst spi_inst_t;
extern spi_inst_t *const spi0;
extern spi_inst_t *const spi1;

typedef enum { SPI_CPOL_0 = 0, SPI_CPOL_1 = 1 } spi_cpol_t;
typedef enum { SPI_CPHA_0 = 0, SPI_CPHA_1 = 1 } spi_cpha_t;
typedef enum { SPI_LSB_FIRST = 0, SPI_MSB_FIRST = 1 } spi_order_t;

uint spi_init(spi_inst_t *spi, uint baudrate);
uint spi_set_baudrate(spi_inst_t *spi, uint baudrate);
void spi_set_format(spi_inst_t *spi, uint data_bits, spi_cpol_t cpol, spi_cpha_t cpha,
    spi_order_t order);
uint spi_get_dreq(spi_inst_t *spi, bool is_tx);
spi_hw_t *spi_get_hw(spi_inst_t *spi);
void spi_deinit(spi_inst_t *spi);
static inline bool spi_is_busy(spi_inst_t *spi) { return (spi_get_hw(spi)->sr & 0x10u) != 0; }

#define SPI_SSPSR_RNE_BITS 0x00000004u

/* ------------------------------------------------------------ hardware/pio */
/* 形状桩（不是寄存器布局）：只保证成员名与 pico-sdk 2.1.0 的 hardware/structs/pio.h 一致 */
typedef struct {
    volatile uint32_t execctrl;
    volatile uint32_t shiftctrl;
    volatile uint32_t addr;
    volatile uint32_t instr;
    volatile uint32_t pinctrl;
} pio_sm_hw_t;

typedef struct {
    volatile uint32_t ctrl, fstat, fdebug, flevel;
    volatile uint32_t txf[4];
    volatile uint32_t rxf[4];
    /* RP2040 上 INSTR_MEM0..31 是**只写**的（读回恒 0，RP2040.svd 标注 ACCESS "WO"），
     * 为了忠实反映这一点，下面这些字段在桩里也只声明、不用于回读。 */
    volatile uint32_t instr_mem[32];
    pio_sm_hw_t sm[4];
} pio_hw_t;

typedef pio_hw_t *PIO;
extern PIO pio0;
extern PIO pio1;

typedef struct {
    uint32_t clkdiv;
    uint32_t execctrl;
    uint32_t shiftctrl;
    uint32_t pinctrl;
} pio_sm_config;

typedef struct pio_program {
    const uint16_t *instructions;
    uint8_t length;
    int8_t origin;
    uint8_t pio_version;
} pio_program_t;

enum pio_src_dest {
    pio_pins = 0,
    pio_x = 1,
    pio_y = 2,
    pio_null = 3,
    pio_pindirs = 4,
    pio_exec = 5,
    pio_pc = 6,
    pio_isr = 7,
    pio_osr = 8,
    pio_status = 9,
};

#define PIO_FDEBUG_TXSTALL_LSB 24u
#define PIO_FDEBUG_RXSTALL_LSB 0u

enum pio_fifo_join {
    PIO_FIFO_JOIN_NONE = 0,
    PIO_FIFO_JOIN_TX = 1,
    PIO_FIFO_JOIN_RX = 2,
};

bool pio_can_add_program(PIO pio, const pio_program_t *program);
uint pio_add_program(PIO pio, const pio_program_t *program);
void pio_remove_program(PIO pio, const pio_program_t *program, uint loaded_offset);
void pio_gpio_init(PIO pio, uint pin);
uint pio_get_dreq(PIO pio, uint sm, bool is_tx);
void pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_restart(PIO pio, uint sm);
void pio_sm_exec(PIO pio, uint sm, uint instr);
void pio_sm_exec_wait_blocking(PIO pio, uint sm, uint instr);
bool pio_sm_is_exec_stalled(PIO pio, uint sm);
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm);
bool pio_sm_is_tx_fifo_full(PIO pio, uint sm);
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t pin_values, uint32_t pin_mask);
void pio_sm_set_pindirs_with_mask(PIO pio, uint sm, uint32_t pin_dirs, uint32_t pin_mask);
uint pio_sm_get_pc(PIO pio, uint sm);
uint pio_sm_get_rx_fifo_level(PIO pio, uint sm);
bool pio_interrupt_get(PIO pio, uint irq);
void pio_interrupt_clear(PIO pio, uint irq);

void sm_config_set_out_pins(pio_sm_config *c, uint out_base, uint out_count);
void sm_config_set_set_pins(pio_sm_config *c, uint set_base, uint set_count);
void sm_config_set_sideset_pins(pio_sm_config *c, uint sideset_base);
void sm_config_set_clkdiv(pio_sm_config *c, float div);
void sm_config_set_out_shift(pio_sm_config *c, bool shift_right, bool autopull, uint pull_threshold);
void sm_config_set_jmp_pin(pio_sm_config *c, uint pin);
void sm_config_set_in_pins(pio_sm_config *c, uint in_base);
void sm_config_set_in_shift(pio_sm_config *c, bool shift_right, bool autopush, uint push_threshold);
void sm_config_set_fifo_join(pio_sm_config *c, enum pio_fifo_join join);
/* 生成头 vfd_scan.pio.h 会用到下面三个 */
pio_sm_config pio_get_default_sm_config(void);
void sm_config_set_wrap(pio_sm_config *c, uint16_t wrap_target, uint16_t wrap);
void sm_config_set_sideset(pio_sm_config *c, uint bit_count, bool optional, bool pindirs);

/* ------------------------------------------------------ pio_instructions */
uint pio_encode_jmp(uint addr);
uint pio_encode_wait_gpio(bool polarity, uint gpio);
uint pio_encode_wait_pin(bool polarity, uint pin);
uint pio_encode_set(enum pio_src_dest dest, uint value);
uint pio_encode_out(enum pio_src_dest dest, uint count);

/* ------------------------------------------------------------ hardware/dma */
typedef enum { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 } dma_channel_transfer_size_t;

typedef struct {
    uint32_t ctrl;
} dma_channel_config;

typedef struct {
    volatile uint32_t read_addr;
    volatile uint32_t write_addr;
    volatile uint32_t transfer_count;
    volatile uint32_t ctrl_trig;
} dma_channel_hw_t;

typedef struct {
    volatile uint32_t ints0; /* 完成中断标志（写 1 清除） */
    dma_channel_hw_t ch[12];
} dma_hw_t;

extern dma_hw_t *const dma_hw;

dma_channel_config dma_channel_get_default_config(uint channel);
void channel_config_set_transfer_data_size(dma_channel_config *c, dma_channel_transfer_size_t size);
void channel_config_set_read_increment(dma_channel_config *c, bool incr);
void channel_config_set_write_increment(dma_channel_config *c, bool incr);
void channel_config_set_dreq(dma_channel_config *c, uint dreq);
void channel_config_set_ring(dma_channel_config *c, bool write, uint size_bits);
void dma_channel_set_read_addr(uint channel, const volatile void *read_addr, bool trigger);
void dma_channel_set_write_addr(uint channel, volatile void *write_addr, bool trigger);
void dma_channel_set_trans_count(uint channel, uint32_t trans_count, bool trigger);
void dma_channel_configure(uint channel, const dma_channel_config *config, volatile void *write_addr,
    const volatile void *read_addr, uint transfer_count, bool trigger);
void dma_channel_abort(uint channel);
bool dma_channel_is_busy(uint channel);
void dma_channel_transfer_from_buffer_now(uint channel, const volatile void *read_addr,
    uint transfer_count);
void dma_channel_cleanup(uint channel);
dma_channel_hw_t *dma_channel_hw_addr(uint channel);
void dma_channel_set_irq0_enabled(uint channel, bool enabled);

/* -------------------------------------------------------------- irq (pico) */
enum { DMA_IRQ_0 = 12 };
typedef void (*irq_handler_t)(void);
void irq_set_exclusive_handler(uint num, irq_handler_t handler);
void irq_set_enabled(uint num, bool enabled);

/* ------------------------------------------------------------ hardware/pwm */
/* 注意：SDK 里没有 pwm_channel_t，而是 enum pwm_chan + 返回 uint 的查询函数
 * （这个差异就是靠真实 SDK 构建发现的） */
enum pwm_chan { PWM_CHAN_A = 0, PWM_CHAN_B = 1 };

typedef struct {
    uint32_t csr, div, top;
} pwm_config;

pwm_config pwm_get_default_config(void);
void pwm_config_set_clkdiv(pwm_config *c, float clkdiv);
void pwm_config_set_wrap(pwm_config *c, uint16_t wrap);
void pwm_init(uint slice_num, pwm_config *c, bool start);
void pwm_set_chan_level(uint slice_num, uint chan, uint16_t level);
void pwm_set_enabled(uint slice_num, bool enabled);
void pwm_set_counter(uint slice_num, uint16_t c);
uint32_t pwm_get_counter(uint slice_num); /* pico-sdk: 读 1 MHz 计数器当前值 */
uint pwm_gpio_to_slice_num(uint gpio);
uint pwm_gpio_to_channel(uint gpio);

/* -------------------------------------------------------------- pico/time */
typedef int32_t alarm_id_t; /* SDK 里 alarm_id_t 就是 int32_t */
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void *user_data);
alarm_id_t add_alarm_in_us(uint64_t us, alarm_callback_t callback, void *user_data, bool fire_if_past);
bool cancel_alarm(alarm_id_t id);

/* --------------------------------------------------------- hardware/timer */
typedef struct {
    volatile uint32_t timehw, timelw, timehr, timelr, alarm0, armed, timerawh, timerawl;
} timer_hw_t;

extern timer_hw_t *const timer_hw;

typedef struct repeating_timer repeating_timer_t;
typedef bool (*repeating_timer_callback_t)(repeating_timer_t *rt);

struct repeating_timer {
    int64_t delay_us;
    repeating_timer_callback_t callback;
    void *user_data;
    void *alarm_pool;
    int64_t alarm_id;
};

bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t callback, void *user_data,
    repeating_timer_t *out);
bool cancel_repeating_timer(repeating_timer_t *timer);

/* ---------------------------------------------------------- hardware/gpio */
enum gpio_function {
    GPIO_FUNC_XIP = 0,
    GPIO_FUNC_SPI = 1,
    GPIO_FUNC_UART = 2,
    GPIO_FUNC_I2C = 3,
    GPIO_FUNC_PWM = 4,
    GPIO_FUNC_SIO = 5,
    GPIO_FUNC_PIO0 = 6,
    GPIO_FUNC_PIO1 = 7,
    GPIO_FUNC_NULL = 31,
};

#define GPIO_OUT 1
#define GPIO_IN 0

void gpio_init(uint gpio);
void gpio_set_dir(uint gpio, bool out);
void gpio_put(uint gpio, bool value);
bool gpio_get(uint gpio);
void gpio_set_function(uint gpio, enum gpio_function fn);
void gpio_set_input_enabled(uint gpio, bool enabled);
void gpio_pull_up(uint gpio);
void gpio_pull_down(uint gpio);

/* -------------------------------------------------------- hardware/clocks */
enum clock_index {
    clk_gpout0 = 0,
    clk_gpout1,
    clk_gpout2,
    clk_gpout3,
    clk_ref,
    clk_sys,
    clk_peri,
    clk_usb,
    clk_adc,
    clk_rtc,
    CLK_COUNT
};

uint32_t clock_get_hz(enum clock_index clk_index);

/* ---------------------------------------------------------- pico/platform */
#define __not_in_flash(group) __attribute__((section(".time_critical." group)))
#define __not_in_flash_func(func_name) __not_in_flash(__STRING(func_name)) func_name
#define __STRING(x) #x
static inline void tight_loop_contents(void) { }

/* ------------------------------------------------------------- pico/time */
typedef uint64_t absolute_time_t;

absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t t);
int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to);
void sleep_ms(uint32_t ms);
void sleep_us(uint64_t us);
uint32_t time_us_32(void);

/* ------------------------------------------------------------ pico/stdlib */
void stdio_init_all(void);

/* stdio 驱动开关：真实构建里 pico_enable_stdio_uart() 只注入 LIB_PICO_STDIO_UART，
 * 本工程自己在 CMake 里注入 VFD_STDIO_UART_ENABLED（默认 1）。桩里按"开"处理。 */
#ifndef VFD_STDIO_UART_ENABLED
#define VFD_STDIO_UART_ENABLED 1
#endif

/* --------------------------------------------------------- hardware/uart */
typedef struct uart_inst uart_inst_t;
extern uart_inst_t *const uart0;
extern uart_inst_t *const uart1;
void uart_write_blocking(uart_inst_t *uart, const uint8_t *src, size_t len);

/* -------------------------------------------------------- pico/multicore */
void multicore_launch_core1(void (*entry)(void));
/* 真实 SDK 里在 hardware/sync.h 定义（跨核内存屏障）；语法检查桩里退化为空 */
static inline void __dmb(void) { }

#endif /* VFD_SDK_STUB_H */
