/*
 * i2c.h - 通用 I2C 总线接口（模型对齐 Linux <linux/i2c.h>，函数名做简化） <drivers/i2c/>
 *
 * 分层（见 docs/drivers.md）：
 *   i2c.h          接口，芯片 / OS 无关
 *   i2c_core.c     通用逻辑：adapter 注册表、i2c_transfer 重试、寄存器读写
 *   i2c_bit.c      通用 bit-bang 时序状态机（START / STOP / 收发字节 / ACK / 时钟延展）
 *   stm32f1_i2c.c  芯片实现：只提供引脚原语 + µs 延时
 *   rtthread_i2c.c OS 适配：总线互斥量 + msh 调试命令
 *   smbus.h/.c     严格 SMBus 协议层（Quick/Send/Receive/Byte/Word/Block/PEC）
 *
 * 本文件是「I2C 总线」层：只管怎么把一串 i2c_msg 送到总线上。
 * 「SMBus 协议」层见 smbus.h —— 那一层才有 PEC、byte-count、小端字序等规范约束。
 *
 * 典型用法（一次「写寄存器地址 + 读一个字节」，AT24C02 / 各种传感器都这样）：
 *
 *     uint8_t reg = 0x10, val;
 *     struct i2c_msg msgs[2] = {
 *         { .addr = 0x50, .flags = 0,        .len = 1, .buf = &reg },
 *         { .addr = 0x50, .flags = I2C_M_RD, .len = 1, .buf = &val },
 *     };
 *     if (i2c_transfer(adap, msgs, 2) != 2) { 失败 }
 *
 * 返回值约定（沿用 Linux 的语义）：
 *   i2c_transfer()      成功返回 msg 个数 num，失败返回负的 I2C_ERR_*
 *   i2c_write()         成功返回字节数，失败返回负值
 *   i2c_read()          成功返回字节数，失败返回负值
 */
#ifndef DRIVERS_I2C_H__
#define DRIVERS_I2C_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define I2C_WEAK    __attribute__((weak))
#else
#define I2C_WEAK
#endif

/* ============================ 错误码 ============================
 * 不用 <errno.h>：本工程是 RT_USING_NANO，没有 POSIX errno。
 * 保持 Linux 的「返回负值」习惯，只是数值自己定义。
 */
#define I2C_ERR_IO          (-1)    /* 通用失败 */
#define I2C_ERR_PARAM       (-2)    /* 参数非法 */
#define I2C_ERR_NACK        (-3)    /* 从机无应答 */
#define I2C_ERR_TIMEOUT     (-4)    /* 等总线超时（含从机长时间拉低 SCL） */
#define I2C_ERR_BUS         (-5)    /* 总线异常（SDA/SCL 释放后仍为低） */

/* ============================ 消息 ============================ */
#define I2C_M_RD            0x0001  /* 该 msg 是读方向 */
#define I2C_M_TEN           0x0010  /* 10 位地址（本实现暂不支持） */
#define I2C_M_IGNORE_NAK    0x1000  /* 忽略 NAK（少见，保留） */
#define I2C_M_REV_DIR_ADDR  0x2000  /* 反转 R/W 位（少见，保留） */
#define I2C_M_NOSTART       0x4000  /* 不发起始位，接着上一段继续 */

/* SMBus Block Read 的长度是「从机在传输中途才给出的」，主机不能提前知道
 * 什么时候该回 NACK。带这个标志的读 msg 由适配器动态处理：
 *   1) 先读 1 个字节（计数 n）并回 ACK，存进 buf[0]
 *   2) 再读 n 个数据字节，只有真正的最后一字节才回 NACK
 *   3) 最后读 msg->len 个尾随字节（SMBus 带 PEC 时为 1，否则为 0）
 * 因此带此标志时 msg->len 表示「尾随字节数」而不是总长度；
 * buf 至少要能放 1 + I2C_RECV_LEN_MAX + msg->len 字节。 */
#define I2C_M_RECV_LEN      0x0400
#define I2C_RECV_LEN_MAX    32u     /* SMBus 块最大 32 字节 */

struct i2c_msg {
    uint16_t addr;      /* 7 位从机地址（不含 R/W 位），如 AT24C02 = 0x50 */
    uint16_t flags;     /* I2C_M_* 组合 */
    uint16_t len;       /* buf 长度；为 0 时只做地址探测 */
    uint8_t *buf;       /* 数据缓冲（读方向也是非 const：硬件会写进来） */
};

/* ============================ 算法 / 适配器 ============================ */
struct i2c_adapter;

/* i2c_algorithm 是「一次完整传输」的入口，对应 Linux 的 master_xfer */
struct i2c_algorithm {
    int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
    uint32_t (*functionality)(struct i2c_adapter *adap);
};

struct i2c_adapter {
    const char *name;                   /* 便于日志，如 "i2c0" */
    const struct i2c_algorithm *algo;   /* 传输算法 */
    void *algo_data;                    /* 算法私有数据（bit-bang 用 i2c_bit_ops） */
    uint16_t retries;                   /* 失败重试次数（0 = 不重试） */
};

/* ============================ bit-bang 原语 ============================
 * i2c_bit.c 只依赖这一组回调，因此换 SoC 时只需再写一份「填 ops」的文件。
 */
struct i2c_bit_ops {
    void *ctx;                          /* 回调私有数据 */
    void (*set_sda)(void *ctx, int level);  /* 1 = 释放（靠上拉变高），0 = 主动拉低 */
    void (*set_scl)(void *ctx, int level);  /* 1 = 释放（靠上拉变高），0 = 主动拉低 */
    int  (*get_sda)(void *ctx);             /* 读 SDA 真实电平 */
    int  (*get_scl)(void *ctx);             /* 读 SCL 真实电平；可为 NULL（不检测延展） */
    void (*delay_us)(void *ctx, unsigned int us);
    unsigned int half_period_us;        /* 一个 SCL 半周期，决定总线速率 */
    unsigned int stretch_timeout_us;    /* SCL 被从机拉低的最长等待，SMBus tTIMEOUT = 25ms */
};

/* i2c_bit.c 导出的算法：把 ops 装进 i2c_adapter.algo_data 即可使用 */
extern const struct i2c_algorithm i2c_bit_algorithm;

/* 总线恢复：从机把 SDA 拉死时，打 9 个 SCL 脉冲让其释放，再补一个 STOP。
 * 返回 0 = 恢复正常，负值 = SCL/SDA 仍被拉低（多半是没上拉或器件坏了）。 */
int i2c_bit_recover(struct i2c_adapter *adap);

/* ============================ 适配器注册 ============================ */
/* 注册一个 adapter，成功返回编号（0 起），失败返回 I2C_ERR_* */
int i2c_add_adapter(struct i2c_adapter *adap);

/* 按编号取 adapter，未注册返回 NULL */
struct i2c_adapter *i2c_get_adapter(unsigned int nr);

/* 取 adapter 的注册编号，未注册返回负值 */
int i2c_adapter_nr(struct i2c_adapter *adap);

/* ============================ 传输 ============================
 *
 * 这一层是按「一次传输有几段」分级的，三级不是重复实现：
 *
 *   i2c_transfer         N 段、任意 flags —— 唯一真正干活的原语
 *    └ i2c_write/read     1 段，写/读一串连续字节
 *       └ i2c_reg_write/read  2 段，「寄存器地址 + 数据」，段间不发起始位
 *
 * 为什么 reg_* 不能删（虽然线上字节等价于「i2c_write 一个多一个字节的缓冲」）：
 *
 *   1) i2c_reg_read 用 i2c_read **根本表达不了**：它必须先写 1 字节寄存器地址，
 *      再发重复起始位把方向转成读（两段）。所以读这边的 reg_* 是必需的，
 *      只留写那半会变成奇怪的接口不对称。
 *
 *   2) i2c_reg_write 在两段之间用 I2C_M_NOSTART 拼成一次连续写，**零拷贝**。
 *      若删掉它，调用方每写一个寄存器都得自己拼
 *          uint8_t tmp[1 + N]; tmp[0] = reg; memcpy(tmp + 1, buf, N);
 *      长度是运行期变量时还得上 VLA 或 malloc。
 *
 *   3) i2c_write 只吃「一段连续缓冲」，它没法替调用方插入寄存器字节；
 *      i2c_reg_write 只吃「reg + 数据」，它没法表达任意 N 段。
 *
 * 选择依据很简单：一串字节直接用 i2c_write/read，带寄存器地址用 i2c_reg_*，
 * 需要「写一段再读一段」或特殊 flags（NOSTART / RECV_LEN）用 i2c_transfer。
 */

/* 执行一组消息。num 条消息之间：第一条发起始位，后续默认发重复起始位
 * （flags 带 I2C_M_NOSTART 则接着上一段继续）。结束时统一发停止位。
 * 成功返回 num。 */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);

/* 单段发送 / 接收（一次性 START..STOP，连续字节流） */
int i2c_write(struct i2c_adapter *adap, uint16_t addr,
              const uint8_t *buf, uint16_t len);
int i2c_read(struct i2c_adapter *adap, uint16_t addr,
             uint8_t *buf, uint16_t len);

/* ============================ 寄存器读写（裸 I2C，不是 SMBus）============================
 * 绝大多数 I2C 器件（AT24C02、普通传感器）都是「先给寄存器地址，再读/写数据」。
 * 这里只是把两段 i2c_msg 拼起来：没有 byte-count 前缀，也没有 PEC。
 *
 * ⚠️ 和 SMBus 的 Block Read 不是一回事：AT24C02 连续读是「地址 + 一串数据」，
 *    中间没有计数字节；SMBus 器件会先回一个 count。真要跟 SMBus 器件通信请用 smbus.h。
 *
 * 读：len 字节写入 buf，成功返回 len；失败返回负的 I2C_ERR_*
 * 写：成功返回 0；失败返回负的 I2C_ERR_*
 */
int i2c_reg_read(struct i2c_adapter *adap, uint16_t addr, uint8_t reg,
                 uint8_t *buf, uint16_t len);
int i2c_reg_write(struct i2c_adapter *adap, uint16_t addr, uint8_t reg,
                  const uint8_t *buf, uint16_t len);

/* 地址探测：只发地址看是否有 ACK。返回 0 = 器件在场，负值 = 不在 */
int i2c_probe_device(struct i2c_adapter *adap, uint16_t addr);

/* ============================ OS 适配钩子 ============================
 * i2c_core.c 里是空实现的 weak 版本；rtthread_i2c.c 用互斥量覆盖，
 * 保证多线程下同一条总线不会互相踩时序。
 */
int  i2c_bus_lock(struct i2c_adapter *adap);
void i2c_bus_unlock(struct i2c_adapter *adap);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_I2C_H__ */
