/*
 * at24c02.h - AT24C02 EEPROM 驱动接口（2Kbit = 256B，8 字节/页） <drivers/eeprom/>
 *
 * 本文件只描述「AT24C02 这个器件怎么用」，不碰寄存器、不依赖 OS。
 * 总线通过 i2c.h 的 struct i2c_adapter 注入，所以换 MCU 不用改这里。
 *
 * ── 两个必须处理的器件特性 ──────────────────────────────────────────────
 *
 *   1) 页写回卷：24C02 的页大小是 8 字节，一次写操作**不能跨页**。
 *      如果从地址 0x07 开始写 4 个字节，器件会在页内回卷 —— 第 9、10 个
 *      字节被写到 0x00、0x01，把那里的原数据覆盖掉。这是 EEPROM 驱动最
 *      常见的 bug。本驱动的 at24c02_write() 会自动按页边界切分，
 *      调用方不需要关心，写任意长度、任意起始地址都是安全的。
 *
 *   2) 写周期 tWR：每写完一页，器件内部要忙最多 5ms，期间对所有地址回
 *      NACK。本驱动用「ACK 轮询」等待（探测到 ACK 即写周期结束），而不是
 *      死等 5ms；典型情况 1~3ms 就返回。轮询本身是总线操作、天然带延时，
 *      因此驱动里没有任何 OS 延时函数，保持 OS 无关。
 *
 * ── 用法 ──────────────────────────────────────────────────────────────
 *
 * 用法（注册成 eeprom_dev，再按字节读写；modules/storage 就吃这一层）：
 *
 *     struct at24c02_dev ee;
 *     struct eeprom_dev  rom;
 *     uint8_t buf[8] = { ... };
 *
 *     at24c02_init(&ee, i2c_get_adapter(0), AT24C02_BOARD_ADDR);
 *     at24c02_register(&rom, &ee, AT24C02_CAPACITY);   // 板级做这一步
 *
 *     eeprom_write(&rom, 0x00, buf, sizeof(buf));      // 自动切页 + 等写周期
 *     eeprom_read (&rom, 0x00, buf, sizeof(buf));      // 顺序读
 *
 * ── 返回值 ────────────────────────────────────────────────────────────
 *     at24c02_read()   成功 EEPROM_OK，失败 EEPROM_ERR_*（见 eeprom.h）
 *     at24c02_write()  成功 EEPROM_OK，失败 EEPROM_ERR_*
 *
 * 注意：同一颗 EEPROM 的并发访问需要调用方自己串行化。总线锁只保证「一次
 *       传输」的原子性，不保证「读-改-写」的原子性。
 */
#ifndef DRIVERS_AT24C02_H__
#define DRIVERS_AT24C02_H__

#include <stddef.h>
#include <stdint.h>

#include "i2c.h"
#include "eeprom.h"     /* 错误码 + eeprom_ops / eeprom_dev */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 器件常量 ============================ */

#define AT24C02_CAPACITY    256u    /* 2Kbit = 256 字节 */
#define AT24C02_PAGE_SIZE   8u      /* 页缓冲 8 字节，跨页会回卷 */

/* ============================ 设备句柄 ============================
 * 和 smbus_dev 一样刻意做得很轻：只记住「挂在哪条总线、地址是多少」。
 */
struct at24c02_dev {
    struct i2c_adapter *adap;
    uint16_t addr;              /* 7 位地址，战舰 V4 上 A0~A2 接地 → 0x50 */
};

static inline void at24c02_init(struct at24c02_dev *dev,
                                struct i2c_adapter *adap, uint16_t addr)
{
    dev->adap = adap;
    dev->addr = addr;
}

/* ============================ 操作 ============================
 *
 * 下面的 at24c02_read / at24c02_write **就是 struct eeprom_ops 的实现**：
 * 首参是 struct eeprom_dev *（注册后的句柄），器件自己的
 * struct at24c02_dev * 从 dev->ctx 里取。所以注册时不需要任何转形状的壳：
 *
 *     const struct eeprom_ops at24c02_eeprom_ops = {
 *         .read = at24c02_read, .write = at24c02_write,
 *     };
 *
 * 它们一般通过 eeprom_read() / eeprom_write() 被调用（那两处已做参数与
 * 越界校验）；直接调用也可以，但传进去的必须是用本器件注册过的 rom。
 */

/* 顺序读：从 addr 起读 len 字节，器件内部地址自动递增（器件自身会回卷，
 * 所以越界在这里就拒绝）。器件物理容量边界由本函数检查：addr + len > 256 →
 * EEPROM_ERR_RANGE；参数非法 → EEPROM_ERR_PARAM；总线失败 → EEPROM_ERR_IO。 */
int at24c02_read(struct eeprom_dev *dev, uint32_t addr, void *buf, size_t len);

/* 按页切分写入：内部逐页写 + 等写周期，跨页安全。
 * 全部写成功才返回 EEPROM_OK；中途失败返回 EEPROM_ERR_IO（已写入的页不会回滚）。 */
int at24c02_write(struct eeprom_dev *dev, uint32_t addr, const void *buf, size_t len);

/* 等器件空闲（ACK 轮询）。返回 0 = 就绪；I2C_ERR_TIMEOUT = 一直没应答。
 * 用途：上电自检、或者绕过本驱动直接写过后确认写周期已结束。
 * 注意 at24c02_write() 内部每页之后都会调用它，正常读写不需要手动调。
 * 这个函数不参与 eeprom_ops，所以首参保持具体类型。 */
int at24c02_wait_ready(struct at24c02_dev *dev);

/* ============================ 注册到 eeprom 适配层 ============================
 * 本器件直接实现 eeprom.h 的 struct eeprom_ops（就是上面的 read / write），
 * 因此注册只需要把它们填进一张表；这张表也叫「本器件的驱动 ops」。
 * 想自己 attach 的话可以直接用：
 *
 *     static struct eeprom_dev rom =
 *         EEPROM_DEV_INITIALIZER(&at24c02_eeprom_ops, &ee, AT24C02_CAPACITY);
 */
extern const struct eeprom_ops at24c02_eeprom_ops;

/* 把本器件注册成 eeprom_dev（「从 0 开始、按字节读写、容量已知」）。
 *   dev       待构造的句柄（由调用方提供存储）；
 *   chip      须已用 at24c02_init() 绑好总线与地址；
 *   capacity  可以小于整片（当分区用），但不能为 0 或超过 AT24C02_CAPACITY。
 * 返回 EEPROM_OK / EEPROM_ERR_PARAM。只做绑定，不发起任何总线传输。
 *
 * 这就是「新增一颗器件要加什么」的答案：器件自己的 .c 里把 read/write 写成
 * ops 的形状 + 导出 register，eeprom.c / eeprom.h 与上层模块都不用改。 */
int at24c02_register(struct eeprom_dev *dev, struct at24c02_dev *chip,
                     uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_AT24C02_H__ */
