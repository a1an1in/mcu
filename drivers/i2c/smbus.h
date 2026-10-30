/*
 * smbus.h - 严格 SMBus 协议层（SMBus 2.0/3.0 §5 的协议集合） <drivers/i2c/>
 *
 * 和 i2c.h 的关系：
 *   i2c.h   —— 「总线怎么走」：i2c_msg / i2c_transfer，只是把字节送上总线。
 *              它不知道 PEC、不知道 byte-count、不管字序。
 *   smbus.h —— 「协议怎么规定」：在 i2c_transfer 之上按 SMBus 规范组织时序。
 *
 * ── 全部 11 种协议，只用两个函数 ────────────────────────────────────────
 * 按线上字节拆开看，SMBus 的协议其实就是「地址 + 若干字节 + 方向」的组合，
 * 差别只有三处：有没有命令码、有没有计数字节、是写一段还是读一段。
 * 前两类用 len 就能完全决定：
 *
 *   smbus_read(dev, cmd, buf, len)              smbus_write(dev, cmd, buf, len)
 *   ──────────────────────────────────────      ───────────────────────────────
 *   cmd = SMBUS_NO_CMD:                         cmd = SMBUS_NO_CMD:
 *     len = 0 → Quick Command（只发地址）         同上
 *     len = 1 → Receive Byte                     len = 1 → Send Byte
 *     len ≥ 2 → 参数错误                          len ≥ 2 → 参数错误
 *
 *   cmd = 命令码:                                cmd = 命令码:
 *     len = 1 → Read Byte                        len = 1 → Write Byte
 *     len = 2 → Read Word（小端）                 len = 2 → Write Word（小端）
 *     len ≥ 3 → Block Read                       len ≥ 3 → Block Write（count = len）
 *               len 是 buf 容量（3..32）                     （3..32）
 *
 * 返回：write 成功返回 0；read 成功返回读到的**数据字节数**（块读时是从机给的
 * count，不含计数字节）。失败返回负的 I2C_ERR_* / SMBUS_ERR_*。
 *
 * 「写一段再读一段」的两种协议参数形状不同，单独保留：
 *   smbus_process_call()        —— Process Call
 *   smbus_block_process_call()  —— Block Write-Block Read Process Call
 *
 * 唯一的表达不了的情况：块长度只有 1~2 字节（规范允许，实际几乎见不到）。
 * 那种边角场景请直接用 i2c_transfer() 拼 msg。
 *
 * ── 规范细节 ─────────────────────────────────────────────────────────
 *   1) 字数据一律「低字节在前」（SMBus 是小端），和 I2C 器件常见的
 *      「寄存器地址 + 高字节在前」不是一回事。
 *   2) 块数据带 byte-count 前缀，计数 1..32。注意这与 AT24C02 那种
 *      「地址 + 连续数据」的裸 I2C 读不同，后者请用 i2c_reg_read()。
 *   3) PEC（Packet Error Code）按规范计算 CRC-8：
 *        多项式 x^8+x^2+x+1 (0x07)，初值 0x00，不反转，无终值异或
 *        覆盖范围 = 整条消息的**所有**字节，包括地址字节和 R/W 位
 *      写方向由主机算出并附加；读方向由从机附加，主机必须校验。
 *   4) 时钟延展超时（tTIMEOUT = 25ms）由底层 bit-bang 检查，超时返回
 *      I2C_ERR_TIMEOUT 并放弃本次传输。
 *
 * 关于总线速率：SMBus 规定 10kHz ~ 100kHz，默认配置 I2C_DELAY_US = 5 即 100kHz。
 *
 * 用法：
 *     struct smbus_dev dev;
 *     uint8_t w[2] = { 0x34, 0x12 };      // 小端写 0x1234
 *     uint8_t r[2];
 *
 *     smbus_dev_init(&dev, adap, 0x2D, SMBUS_F_PEC);
 *     smbus_write(&dev, 0x10, w, 2);      // Write Word
 *     smbus_read(&dev, 0x10, r, 2);       // Read Word -> r[0]=lo, r[1]=hi
 *
 *     smbus_write(&dev, SMBUS_NO_CMD, &d, 1);   // Send Byte
 *     smbus_read(&dev, SMBUS_NO_CMD, &d, 1);    // Receive Byte
 */
#ifndef DRIVERS_SMBUS_H__
#define DRIVERS_SMBUS_H__

#include <stdint.h>

#include "i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 常量 ============================ */

/* 块数据的最大字节数（SMBus 规范固定为 32） */
#define SMBUS_BLOCK_MAX         32u

/* 传给 smbus_read/smbus_write 的 cmd：表示这条协议没有命令码。
 * 取 -1 而不是 0，因为 0x00 是合法命令码（AT24C02 的地址 0 就用它）。 */
#define SMBUS_NO_CMD            (-1)

/* 设备标志 */
#define SMBUS_F_PEC             0x0001u /* 该器件支持/要求 PEC */

/* ============================ 错误码 ============================
 * 在 I2C_ERR_*（-1..-5）基础上追加 SMBus 特有的语义错误。
 */
#define SMBUS_ERR_PROTO         (-6)    /* 该器件不支持这个协议/组合 */
#define SMBUS_ERR_COUNT         (-7)    /* 块长度非法（> 32 或超过缓冲区） */
#define SMBUS_ERR_PEC           (-8)    /* PEC 校验失败（数据在传输中被破坏） */

/* ============================ 设备句柄 ============================
 * 只有一个 7 位地址和几个标志，故意做得很轻（对齐 Linux 的 i2c_client）。
 * PEC 是「器件属性」而不是「总线属性」：同一条总线上可以既有要 PEC 的器件，
 * 也有不支持的器件，所以标志挂在设备上。
 */
struct smbus_dev {
    struct i2c_adapter *adap;
    uint16_t addr;              /* 7 位地址 */
    uint16_t flags;             /* SMBUS_F_* */
};

static inline void smbus_dev_init(struct smbus_dev *dev,
                                  struct i2c_adapter *adap,
                                  uint16_t addr, uint16_t flags)
{
    dev->adap  = adap;
    dev->addr  = addr;
    dev->flags = flags;
}

/* ============================ PEC ============================ */

/* CRC-8 累积计算。可以分多次调用把多段字节串起来：
 *     crc = smbus_pec(0, seg1, n1);
 *     crc = smbus_pec(crc, seg2, n2);
 */
uint8_t smbus_pec(uint8_t crc, const uint8_t *buf, unsigned int len);

/* 自检：用 CRC-8/SMBUS 的标准校验值（"123456789" → 0xF4）验证算法。
 * 返回 0 = 通过，SMBUS_ERR_PEC = 不通过。 */
int smbus_selftest(void);

/* ============================ 统一的读 / 写 ============================ */

/* 读。cmd = SMBUS_NO_CMD 表示不发命令码（Quick / Receive Byte）。
 * 返回值：读到的数据字节数（≥0），失败为负值。 */
int smbus_read(struct smbus_dev *dev, int cmd, uint8_t *buf, uint16_t len);

/* 写。cmd = SMBUS_NO_CMD 表示不发命令码（Quick / Send Byte）。
 * 返回值：0 成功，负值失败。 */
int smbus_write(struct smbus_dev *dev, int cmd, const uint8_t *buf, uint16_t len);

/* ============================ 写一段 + 读一段 ============================ */

/* Process Call：写一个字，紧接着重复起始位读一个字（都是小端）。 */
int smbus_process_call(struct smbus_dev *dev, uint8_t cmd,
                       uint16_t wdata, uint16_t *rdata);

/* Block Write-Block Read Process Call：先写一块，紧接着读一块。 */
int smbus_block_process_call(struct smbus_dev *dev, uint8_t cmd,
                             const uint8_t *wdata, uint8_t wcount,
                             uint8_t *rdata, uint8_t *rcount);

/* ============================ SMBus Alert Response ============================
 * 收到 SMBALERT# 后主机读保留地址 0x0C，从机回「地址<<1 | 1」，右移即 7 位地址。
 * 它本质就是「对 0x0C 做 Receive Byte」，所以做成 inline，不单独占目标码。
 */
static inline int smbus_alert_response(struct i2c_adapter *adap, uint8_t *addr)
{
    struct smbus_dev ara;
    uint8_t raw = 0;
    int ret;

    if (adap == 0 || addr == 0)
    {
        return I2C_ERR_PARAM;
    }

    smbus_dev_init(&ara, adap, 0x0Cu, 0);
    ret = smbus_read(&ara, SMBUS_NO_CMD, &raw, 1);
    if (ret < 0)
    {
        return ret;
    }

    *addr = (uint8_t)(raw >> 1);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_SMBUS_H__ */
