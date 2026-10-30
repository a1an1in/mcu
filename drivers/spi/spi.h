/*
 * spi.h - 通用 SPI 总线接口（模型对齐 Linux <linux/spi/spi.h>，函数名简化） <drivers/spi/>
 *
 * 分层（详见 docs/drivers.md 的四维模型）：
 *   spi.h         接口，芯片 / OS 无关
 *   spi_core.c    通用逻辑：控制器注册表、message 组装与校验、同步传输
 *   spi_config.h  板级事实：本板用哪路 SPI、引脚
 *   stm32f1_spi.c 芯片实现：STM32F1 的 SPI1/2/3 寄存器 + CS 引脚
 *
 * 没有 OS 适配文件：总线的互斥量直接用 OSAL 的 osal_mutex_* 实现（见
 * spi_core.c），文件里不含任何 RTOS 头，换 OS 只换 osal 后端。
 *
 * ── 和 Linux 的对应关系 ───────────────────────────────────────────────
 *   本文件                     Linux
 *   struct spi_transfer        struct spi_transfer        一段收发
 *   struct spi_message         struct spi_message         N 段 + CS 时隙
 *   struct spi_device          struct spi_device          挂在总线上的从设备
 *   struct spi_controller      struct spi_controller      一条 SPI 总线
 *   spi_message_init()         spi_message_init()
 *   spi_message_add_tail()     spi_message_add_tail()
 *   spi_sync()                 spi_sync()
 *   spi_write()                spi_write()
 *   spi_read()                 spi_read()
 *   spi_write_then_read()      spi_write_then_read()
 *
 * 返回值沿用 Linux 的 SPI 约定：成功返回 0，失败返回负的 SPI_ERR_*。
 * （注意 I2C 那边是「成功返回字节数」——那是本工程自己的约定；SPI 这里
 *   刻意照 Linux，因为实际搬了多少字节已经在 msg->actual_length 里了。）
 *
 * ── 三个必须知道的语义 ────────────────────────────────────────────────
 * 1) 一次 spi_sync = 一个 CS 时隙：进入时拉低 CS，退出时释放，中间所有
 *    transfer 都在 CS 有效期内完成。这就是 SPI 与 I2C 最大的不同：
 *    片选不属于总线，属于设备。
 *
 * 2) tx_buf / rx_buf 可以只给一个：
 *      tx != NULL, rx == NULL  → 只发，收进来的字节丢掉
 *      tx == NULL, rx != NULL  → 只收，发出去的是 0xFF
 *      两者都给                → 全双工，同一拍收发的字节
 *
 * 3) 段长 len == 0 的 transfer 不碰硬件，只当「延时占位」（配合
 *    delay_usecs 用），与 Linux 一致。
 *
 * ── 典型用法 ─────────────────────────────────────────────────────────
 *     // 板上有一颗 "写命令 + 读 N 字节" 的器件，CS = PB12
 *     struct spi_device dev = {
 *         .ctlr = spi_get_controller(0),
 *         .max_speed_hz = 4000000u,
 *         .mode = SPI_MODE_0,
 *         .cs_pin = SPI_CS_PIN(1, 12),
 *     };
 *
 *     uint8_t cmd = 0x03, buf[8];
 *     spi_write_then_read(&dev, &cmd, 1, buf, sizeof(buf));   // 0 = 成功
 *
 * 需要一次 CS 内多段（例如「命令 / 10µs / 数据」）时用 message：
 *
 *     struct spi_message msg;
 *     struct spi_transfer x[2] = {
 *         { .tx_buf = &cmd, .len = 1, .delay_usecs = 10 },
 *         { .rx_buf = buf,  .len = sizeof(buf) },
 *     };
 *     spi_message_init(&msg);
 *     spi_message_add_tail(&x[0], &msg);
 *     spi_message_add_tail(&x[1], &msg);
 *     spi_sync(&dev, &msg);
 */
#ifndef DRIVERS_SPI_H__
#define DRIVERS_SPI_H__

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 错误码 ============================
 * 与 i2c.h 一样不用 <errno.h>：本工程是 RT_USING_NANO，没有 POSIX errno。
 */
#define SPI_ERR_IO          (-1)    /* 通用失败 */
#define SPI_ERR_PARAM       (-2)    /* 参数非法 */
#define SPI_ERR_NOMEM       (-3)    /* message 里装不下更多 transfer */
#define SPI_ERR_TIMEOUT     (-4)    /* 等 TXE / RXNE 超时 */
#define SPI_ERR_BUSY        (-5)    /* 总线被占用 / 加锁失败 */

/* ============================ 模式位 ============================ */
#define SPI_CPHA            0x01
#define SPI_CPOL            0x02
#define SPI_CS_HIGH         0x04    /* 片选高有效（多数器件是低有效） */
#define SPI_LSB_FIRST       0x08    /* 低位先发（默认高位先发） */

#define SPI_MODE_0          (0 | 0)
#define SPI_MODE_1          (0 | SPI_CPHA)
#define SPI_MODE_2          (SPI_CPOL | 0)
#define SPI_MODE_3          (SPI_CPOL | SPI_CPHA)

/* ============================ CS 引脚编码 ============================
 * 片选是「设备」的属性而不是总线的属性，所以放在 spi_device 里。
 * 编码与 gpio.h 的 GPIO_PIN(port,pin) 同形：port 0..4 = A..E，pin 0..15。
 * SPI_CS_NONE 表示这条设备不需要（或不能由软件控制）片选 —— 例如 CS 被
 * 硬件直接接地，或者只是拿总线做回环自测。
 */
#define SPI_CS_PIN(port, pin)   ((uint32_t)((((uint32_t)(port)) << 8) | ((pin) & 0xFFu)))
#define SPI_CS_NONE             0xFFFFFFFFu

/* ============================ 一段传输 ============================ */
struct spi_transfer {
    const void *tx_buf;         /* 发什么；NULL = 只收（发 0xFF） */
    void       *rx_buf;         /* 收到哪；NULL = 只发（收到的丢掉） */
    uint16_t    len;            /* 本段字节数；0 = 不碰硬件，只做延时占位 */
    uint32_t    speed_hz;       /* 0 = 用 spi_device.max_speed_hz */
    uint16_t    delay_usecs;    /* 本段结束后延时，再发下一段 */
    uint8_t     cs_change;      /* 1 = 本段结束后放开 CS，下一段重新拉低 */
    uint8_t     bits_per_word;  /* 0 = 8；STM32F1 只支持 8 / 16 */
};

/* ============================ 一条消息 ============================
 * 与 Linux 的差异（刻意的）：
 *   Linux 的 transfer 是链表节点，spi_message_add_tail() 只挂指针，调用方
 *   必须在传输结束前保证 transfer 还活着。这里改成**定长数组 + 按值拷贝**：
 *   段数上限 SPI_MSG_MAX_XFER，调用方可以把 message 和 transfer 都放栈上，
 *   不会有「栈帧已经返回、链表节点还挂在消息里」这类悬垂问题。
 *   段数不够时 add_tail() 返回 SPI_ERR_NOMEM，不会静默丢段。
 */
#ifndef SPI_MSG_MAX_XFER
#define SPI_MSG_MAX_XFER        4u
#endif

struct spi_message {
    struct spi_device *spi;                     /* 由 spi_sync 填 */
    struct spi_transfer xfer[SPI_MSG_MAX_XFER]; /* 按加入顺序执行 */
    uint8_t  n;                                 /* 已加入的段数 */
    uint32_t actual_length;                     /* 本次实际搬了多少字节 */
    int      status;                            /* 0 / SPI_ERR_*，spi_sync 写入 */
};

/* 初始化（等价 Linux 的 spi_message_init） */
static inline void spi_message_init(struct spi_message *msg)
{
    memset(msg, 0, sizeof(*msg));
}

/* 追加一段到末尾。成功 0，段数超上限返回 SPI_ERR_NOMEM。
 * 参数顺序与 Linux 一致：(transfer, message)。 */
static inline int spi_message_add_tail(const struct spi_transfer *xfer,
                                       struct spi_message *msg)
{
    if (xfer == NULL || msg == NULL)
    {
        return SPI_ERR_PARAM;
    }
    if (msg->n >= SPI_MSG_MAX_XFER)
    {
        return SPI_ERR_NOMEM;
    }
    msg->xfer[msg->n++] = *xfer;
    return 0;
}

/* ============================ 设备 / 控制器 ============================ */
struct spi_controller;

/* 从设备：板上每颗挂在 SPI 上的芯片对应一个（CS / 速率 / 模式各不相同） */
struct spi_device {
    struct spi_controller *ctlr;    /* 挂在哪条总线上 */
    uint32_t max_speed_hz;          /* 该器件能接受的最高 SCLK；0 = 用最慢档（调试安全） */
    uint8_t  mode;                  /* SPI_MODE_x | SPI_CS_HIGH | SPI_LSB_FIRST */
    uint8_t  bits_per_word;         /* 0 = 8 */
    uint32_t cs_pin;                /* SPI_CS_PIN(port,pin) 或 SPI_CS_NONE */
};

/* 控制器算法：SoC 实现填这三个回调（都能为 NULL，除 transfer_one） */
struct spi_algorithm {
    /* 应用 mode / 速率 / 位宽；每次 spi_sync 开头调用一次，实现应当幂等 */
    int  (*setup)(struct spi_controller *ctlr, struct spi_device *spi);
    /* 搬一段字节：tx/rx 至少一个非空，len > 0；返回 0 / SPI_ERR_* */
    int  (*transfer_one)(struct spi_controller *ctlr, struct spi_device *spi,
                         struct spi_transfer *xfer);
    /* 片选：active = 1 选中，0 释放（极性由设备 mode 的 SPI_CS_HIGH 决定） */
    void (*set_cs)(struct spi_controller *ctlr, struct spi_device *spi, int active);
};

struct spi_controller {
    const char *name;                       /* 便于日志，如 "spi0" */
    const struct spi_algorithm *algo;       /* 传输算法 */
    void *algo_data;                        /* 算法私有数据（寄存器基址 / 时钟等） */
    uint32_t max_speed_hz;                  /* 本总线能跑的最高 SCLK */
    /* 段间延时用；NULL 表示忽略 delay_usecs（只做占位） */
    void (*delay_us)(void *ctx, unsigned int us);
    void *delay_ctx;
};

/* ============================ 控制器注册 ============================ */
/* 最多同时注册几条总线（够用就好，RAM 里就是一张指针表） */
#ifndef SPI_MAX_CONTROLLERS
#define SPI_MAX_CONTROLLERS     4
#endif

/* 注册一条总线，成功返回编号（0 起），失败返回 SPI_ERR_* */
int spi_add_controller(struct spi_controller *ctlr);

/* 按编号取总线，未注册返回 NULL */
struct spi_controller *spi_get_controller(unsigned int nr);

/* 取总线的注册编号，未注册返回负值 */
int spi_controller_nr(struct spi_controller *ctlr);

/* ============================ 传输 ============================
 * 三级不是重复实现：
 *   spi_sync              N 段 + 一个 CS 时隙 —— 唯一真正干活的原语
 *    └ spi_write/read      1 段，纯发 / 纯收
 *       └ write_then_read  2 段，「先发命令再收数据」，CS 全程有效
 *
 * 单段就能解决的事不要拼 message；反过来，需要「同一 CS 内先写后读」时
 * 必须用 message，分成两次 spi_sync 会在中间释放 CS，多数器件会当成两次
 * 独立事务。
 */

/* 执行一条消息。进入时拉 CS、退出时释放；段间按 delay_usecs / cs_change
 * 处理。成功返回 0，失败返回负的 SPI_ERR_*（同时写进 msg->status）。 */
int spi_sync(struct spi_device *spi, struct spi_message *msg);

/* 单段发送 / 接收。成功返回 0。 */
int spi_write(struct spi_device *spi, const void *buf, uint16_t len);
int spi_read(struct spi_device *spi, void *buf, uint16_t len);

/* 同一 CS 内「先发 n_tx 字节，再收 n_rx 字节」—— 各种「写命令/地址再读」
 * 的器件（Flash、传感器）最常用的一条。成功返回 0。 */
int spi_write_then_read(struct spi_device *spi,
                        const void *txbuf, uint16_t n_tx,
                        void *rxbuf, uint16_t n_rx);

/* ============================ 总线锁 ============================
 * 一次 spi_sync 期间独占整条总线：SCLK / MOSI 是共享的，两个线程交错
 * 会在别人的片选期里塞字节。
 *
 * 实现用 OSAL 的 osal_mutex_*（见 spi_core.c），所以这里不需要 weak 钩子，
 * 也不需要 <os>_spi.c —— 换 RTOS 只换 osal 后端。互斥量在
 * spi_add_controller() 里建好，正常调用 spi_sync 时自动加解锁。
 *
 * ⚠️ 这两个函数只用于「上层想自己把总线踩住一段时间」的场景，且此时
 * **不能再调用 spi_sync**（会二次加锁，非递归互斥量会死锁）。
 *
 * ⚠️ 同理，**不要在 rt_hw_board_init 阶段访问总线**：那时调度器还没起来，
 *    osal_mutex_lock() 会去写一个还不存在的线程控制块（板上表现为
 *    BusFault/IMPRECISERR 直接死机）。板级只负责 spi_add_controller()
 *    与建设备，第一次真正的传输留到 main / 线程里。
 */
int  spi_bus_lock(struct spi_controller *ctlr);
void spi_bus_unlock(struct spi_controller *ctlr);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_SPI_H__ */
