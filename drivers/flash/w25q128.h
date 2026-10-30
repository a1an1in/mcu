/*
 * w25q128.h - W25Q128 / NM25Q128 SPI NOR FLASH 驱动接口 <drivers/flash/>
 *
 * 分层：本文件只描述「25Q128 这颗片子怎么用」，不碰寄存器、不依赖 OS。
 * 总线通过 spi.h 的 struct spi_controller 注入（片选由 struct spi_device 携带），
 * 所以换 MCU 不用改这里。
 *
 * ── NOR FLASH 与 EEPROM 最大的不同：写之前必须先擦 ─────────────────────
 *
 *   NOR 的存储单元只能「1 → 0」。擦除是把整块变回 1（0xFF），写入是把选中的
 *   位从 1 拉成 0。所以：
 *
 *     - 一片新片子 / 刚擦过的区域（全 0xFF）可以直接写；
 *     - 已经写过数据的地方，要把某位从 0 改回 1 —— 做不到，必须先擦整个扇区。
 *
 *   擦除的最小单位是扇区（4KB），不是字节。想改 4KB 里的 10 个字节，得
 *   「读出 4KB → 改 → 擦扇区 → 整块写回」。
 *
 *   本驱动提供两级：
 *     w25q128_write()        只做写入，**要求目标区域已经擦过**（像 memcpy，
 *                            但只能 1→0）。自动按 256 字节页切分，跨页安全。
 *     w25q128_write_erase()  读-改-写，自己处理擦除。需要调用方给一块
 *                            ≥ 4KB 的 scratch（驱动里不常驻 4KB 静态 RAM）。
 *
 * ── 页回卷（和 AT24C02 同一类坑）───────────────────────────────────────
 *   一次页编程最多 256 字节，且**不能跨页**：从 0x0F0 写 32 字节，后 16 个
 *   字节会回卷到 0x000，把页首的数据覆盖掉。w25q128_write() 内部按页切分，
 *   调用方不用关心起始地址是否对齐。
 *
 * ── 忙等待 ────────────────────────────────────────────────────────────
 *   擦除/编程期间状态寄存器 SR1 的 BUSY 位为 1。本驱动用「反复读状态」轮询，
 *   每次轮询本身就是一次完整 SPI 事务（几 µs），所以驱动里没有任何 OS 延时，
 *   保持 OS 无关。扇区擦除典型 45ms / 最大 400ms，整片擦除要几十秒。
 *
 * 返回值：成功 0；失败为负值 —— 总线错误直接返回 SPI_ERR_*，本层追加
 *         W25Q_ERR_*（见下）。
 */
#ifndef DRIVERS_W25Q128_H__
#define DRIVERS_W25Q128_H__

#include <stdint.h>

#include "spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 器件常量 ============================ */

#define W25Q_PAGE_SIZE      256u        /* 页编程单位，跨页会回卷 */
#define W25Q_SECTOR_SIZE    4096u       /* 擦除最小单位 */
#define W25Q_BLOCK_SIZE     65536u      /* 块擦除单位（本驱动未用到，留作参考） */

/* SR1 位 */
#define W25Q_SR1_BUSY       0x01u
#define W25Q_SR1_WEL        0x02u

/* 追加在 SPI_ERR_*(-1..-5) 之后的器件层错误 */
#define W25Q_ERR_RANGE      (-6)    /* addr + len 超出容量 */
#define W25Q_ERR_ID         (-7)    /* 读不到 JEDEC ID / 容量码不认识 */
#define W25Q_ERR_TIMEOUT    (-8)    /* BUSY 一直不落 */
#define W25Q_ERR_ALIGN      (-9)    /* 扇区擦除地址没对齐到 4KB */

/* ============================ 设备句柄 ============================
 * 内嵌一个 struct spi_device：片选 / 模式 / 速率都是「这颗片子」的属性，
 * 与总线无关。板级只需给出「挂在哪条总线、片选是哪个脚」。
 */
struct w25q128_dev {
    struct spi_device spi;      /* 由 w25q128_init() 填好 */
    uint32_t capacity;          /* 字节数，由 JEDEC 容量码推出来；**探测前为 0**，
                                 * 所以 init 之后别拿它算地址，先 w25q128_probe()
                                 * 或直接调读写接口（内部会自动探测） */
    uint32_t jedec;             /* 24 位：厂商 + 类型 + 容量码 */
};

/* ============================ 初始化 ============================
 *   ctlr    spi_get_controller(n) 取到的总线
 *   cs_pin  SPI_CS_PIN(port, pin)，战舰 V4 上是 PB12
 *   速率 / 模式取 w25q128_config.h（板级事实）
 *
 * ⚠️ init **不碰总线**，只把这几项记下来。原因是它通常在 rt_hw_board_init
 * 阶段被调用，那时 RT-Thread 调度器还没起来：一次 SPI 传输要加总线锁，
 * 而 osal_mutex_lock() 会去写一个还不存在的线程控制块 —— 板上表现就是
 * BusFault/IMPRECISERR 直接死机。所以真正的探测推迟到 w25q128_probe()
 * （第一次读写/擦除时会自动调用一次）。
 *
 * 返回 0，或参数非法时的 SPI_ERR_PARAM。
 */
int w25q128_init(struct w25q128_dev *dev, struct spi_controller *ctlr,
                 uint32_t cs_pin);

/* ============================ 探测 ============================
 * 读 JEDEC ID、算出容量，并等上一次擦除结束。第一次访问总线时自动调用，
 * 也可以显式调（`flash info` 就是这么干的）。
 *
 * 读不到、或是 256Mbit 以上（需要 4 字节地址模式，本驱动未实现）返回
 * W25Q_ERR_ID；总线错误返回 SPI_ERR_*。成功返回 0，之后 dev->capacity 有效。
 */
int w25q128_probe(struct w25q128_dev *dev);

/* ============================ 读 ============================
 * 从 addr 起读 len 字节到 buf。不需要擦除，可以任意长度、任意地址。
 * 成功 0，失败负值。
 */
int w25q128_read(struct w25q128_dev *dev, uint32_t addr, void *buf, uint32_t len);

/* ============================ 写（要求已擦除）============================
 * 从 addr 起写 len 字节。**目标区域必须已经是 0xFF**（NOR 只能 1→0），
 * 否则写进去的位会与旧数据相与，结果不是你要的值，而且驱动无法察觉。
 * 内部按 256 字节页切分，跨页安全。成功 0，失败负值。
 */
int w25q128_write(struct w25q128_dev *dev, uint32_t addr,
                  const void *buf, uint32_t len);

/* ============================ 读-改-写（自带擦除）============================
 * 想在已经写过数据的地方改几个字节时用这个：
 *   逐扇区「读出 4KB → 改 → 擦除 → 写回」，可以跨扇区。
 *
 *   scratch 由调用方提供，至少要 W25Q_SECTOR_SIZE 字节 —— 驱动里不放 4KB
 *   静态缓冲，谁用谁出这块内存（用完还能还回去）。
 *
 *   扇区内内容没变化时会跳过擦写，省寿命。
 * 成功 0，失败负值。
 */
int w25q128_write_erase(struct w25q128_dev *dev, uint32_t addr,
                        const void *buf, uint32_t len, void *scratch);

/* ============================ 擦除 ============================ */

/* 擦除 addr 所在的扇区。addr 必须 4KB 对齐，否则返回 W25Q_ERR_ALIGN
 * （不替调用方取整：擦错扇区是不可恢复的）。成功 0。 */
int w25q128_erase_sector(struct w25q128_dev *dev, uint32_t addr);

/* 整片擦除。16MB 全片擦除要几十秒，本函数会一直等到 BUSY 落下；
 * 调试时请确保串口终端不会先超时。成功 0。 */
int w25q128_erase_chip(struct w25q128_dev *dev);

/* ============================ 状态 ============================ */

/* 读状态寄存器 1（bit0 = BUSY，bit1 = WEL）。读失败返回 0xFF。 */
uint8_t w25q128_read_status(struct w25q128_dev *dev);

/* 等 BUSY 落下。返回 0 = 就绪，W25Q_ERR_TIMEOUT = 超时。
 * w25q128_write/erase 内部每次都会调用，正常使用不必手动调。 */
int w25q128_wait_ready(struct w25q128_dev *dev);

/* ============================ 默认实例 ============================
 * 板级初始化后注册一次，上层（msh 的 flash 命令等）就不用自己持有句柄。
 * 与 eeprom_storage 的 set/get_default 同一套写法。
 */
void w25q128_set_default(struct w25q128_dev *dev);
struct w25q128_dev *w25q128_get_default(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_W25Q128_H__ */
