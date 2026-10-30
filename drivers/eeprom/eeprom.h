/*
 * eeprom.h - 字节级 EEPROM 访问接口（适配层抽象） <drivers/eeprom/>
 *
 * 定位：给上层模块一块「从 0 开始、按字节读写、容量已知」的 EEPROM 视图。
 * 上层只认识字节语义，不认识页大小、写周期、从机地址、字地址宽度。
 *
 * 分层：
 *   modules/storage/eeprom_storage.c   只依赖本头（芯片 / 总线 / OS 无关）
 *              │  struct eeprom_dev *，调 eeprom_read() / eeprom_write()
 *              ▼
 *   drivers/eeprom/eeprom.c            通用核心：参数 / 越界校验 + 分发
 *              │  struct eeprom_ops
 *              ▲   （器件实现 ops，并通过 xxx_register() 注册进来）
 *   drivers/eeprom/at24c02.c           器件协议（页切分 / ACK 轮询）+ at24c02_register()
 *
 * ── 为什么是 ops 表，而不是「构造函数里写死器件类型」 ──────────────────
 *
 *   旧接口是 eeprom_init(dev, struct at24c02_dev *chip, capacity)：
 *   抽象层的构造函数签名里出现了具体器件类型，于是
 *     - 换器件（24C64 / FRAM / 内部 Flash）必须改 eeprom.h + eeprom.c，
 *       或者再加一个 eeprom_init_xxx()，所有调用方还都得 include 器件头；
 *     - 核心层里出现 (struct at24c02_dev *)ctx 强转 —— 抽象是假的；
 *     - 参数检查 / 越界检查 / 错误码翻译在每个器件的适配里各写一遍。
 *
 *   现在核心只认 struct eeprom_ops。**新增器件 = 器件自己的 .c 里把读写写成
 *   ops 的形状，再导出一个 register**（at24c02.c 就是这么做的，没有额外的
 *   适配文件、也没有转形状的壳）：
 *
 *       int at24c64_read(struct eeprom_dev *dev, uint32_t addr,
 *                        void *buf, size_t len)
 *       {
 *           struct at24c64_dev *chip = (struct at24c64_dev *)dev->ctx;
 *           ...
 *       }
 *
 *       const struct eeprom_ops at24c64_eeprom_ops = {
 *           .read = at24c64_read, .write = at24c64_write,
 *       };
 *
 *       int at24c64_register(struct eeprom_dev *dev, struct at24c64_dev *chip,
 *                            uint32_t capacity)
 *       {
 *           return eeprom_register(dev, &at24c64_eeprom_ops, chip, capacity);
 *       }
 *
 *   为什么 ops 的首参是 struct eeprom_dev * 而不是 void *：
 *     - 类型明确，编译器能挡住「传错句柄」这类错误；
 *     - 器件要拿自己的私有数据时从 dev->ctx 取（即注册时传进来的 chip），
 *       等价于 Linux 里 file_operations 通过 filp->private_data 取私有结构；
 *     - 函数指针类型与 ops 逐项一致，注册时不需要任何强转或包装。
 *
 *   本文件、eeprom.c、以及所有上层模块**一行都不用改**。RAM / 内部 Flash /
 *   FRAM / 掉电保存区同理 —— 只要能提供这两个函数，本层就认。
 *
 * 约定：
 *   - 地址从 0 开始、单位字节；越界由核心层（eeprom_read / eeprom_write）
 *     拒绝，适配者不必重复检查，也不依赖调用方自觉；
 *   - 返回 EEPROM_OK = 成功，负值 = 失败（EEPROM_ERR_*）；read 成功时不返回
 *     字节数（要读多少由调用方给定）；
 *   - 后端 ops 拿到的是**裸搬运**请求：空指针与 [0, capacity) 越界已由核心
 *     层挡掉，len 一定 > 0；
 *   - 总线锁只保证「一次传输」的原子性，「读-改-写」的原子性由上层自己
 *     的互斥量负责，本层不做。
 *
 * 本文件不含任何 OS 头，两个 RTOS 后端都能编。
 */
#ifndef DRIVERS_EEPROM_H__
#define DRIVERS_EEPROM_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 返回值 ============================ */

#define EEPROM_OK           0       /* 成功 */
#define EEPROM_ERR_PARAM    (-1)    /* 参数非法（空指针 / 容量为 0 / 后端缺操作） */
#define EEPROM_ERR_RANGE    (-2)    /* addr + len 超出 capacity */
#define EEPROM_ERR_IO       (-3)    /* 器件读写失败（总线 / 器件不应答） */

/* ============================ 后端契约 ============================
 * 「后端」= 一颗具体器件 / 一块 RAM / 一段内部 Flash 的适配实现。
 * 两个函数只负责把 [addr, addr+len) 这一段原始字节搬进搬出：
 *   - dev 就是注册时那个 eeprom_dev；器件自己的私有句柄在 dev->ctx 里
 *     （等价 Linux 的 filp->private_data）；
 *   - addr 已由核心层校验在 [0, capacity) 内，len > 0；
 *   - 成功返回 EEPROM_OK，失败返回 EEPROM_ERR_*；
 *   - 不要把底层错误码（I2C_ERR_* 等）透传出来 —— 上层只认识 EEPROM_ERR_*，
 *     否则换器件就得连带改上层的判断。
 */
struct eeprom_dev;      /* 前置声明：ops 的首参就是它 */

struct eeprom_ops {
    int (*read)(struct eeprom_dev *dev, uint32_t addr, void *buf, size_t len);
    int (*write)(struct eeprom_dev *dev, uint32_t addr, const void *buf, size_t len);
};

/* ============================ 设备句柄 ============================
 * 刻意做得很轻：只记「怎么读写、是谁的上下文、多大」。
 * 上层只用 eeprom_read / eeprom_write，不需要碰 ops。
 */
struct eeprom_dev {
    const struct eeprom_ops *ops;   /* 后端操作表；生命周期须长于本 dev */
    void *ctx;                      /* 后端私有上下文（如 struct at24c02_dev *） */
    uint32_t capacity;              /* 可用字节数，从地址 0 算起 */
};

/* 静态初始化（板级 / 测试里可以省掉一次 attach）：
 *
 *     static const struct eeprom_ops my_ops = { .read = ..., .write = ... };
 *     static struct eeprom_dev rom = EEPROM_DEV_INITIALIZER(&my_ops, ctx, 256u);
 *
 * 注意：这个宏不做校验。想少写一个函数就用它，但 ops / capacity 由你自己
 * 保证合法；拿不准就改用 eeprom_register() 或器件提供的 xxx_register()
 * （例如 at24c02_register()——器件的 ops 是私有常量，不对外暴露）。 */
#define EEPROM_DEV_INITIALIZER(_ops, _ctx, _cap) \
    { .ops = (_ops), .ctx = (_ctx), .capacity = (_cap) }

/* ============================ 构造 ============================
 * 把「后端操作表 + 上下文 + 容量」绑到 dev 上。
 * ops 的 read / write 都必须非空，capacity 必须 > 0（容量语义是调用方的事，
 * 本层只挡「明显不可能」的值）。
 * 返回 EEPROM_OK / EEPROM_ERR_PARAM。
 *
 * 器件专用的便捷入口由器件文件自己提供（如 at24c02_register()），
 * 它们内部就是调用本函数，只是额外校验器件自身的容量上限。 */
int eeprom_register(struct eeprom_dev *dev, const struct eeprom_ops *ops,
                      void *ctx, uint32_t capacity);

/* 句柄是否可用（dev 非空、ops 及其 read/write 都非空、capacity > 0）。
 * 返回 1 = 可读可写，0 = 不可用。
 * 上层模块 init 时用一次就能挡掉「只构造了一半」的句柄，不必逐个判空。 */
int eeprom_dev_valid(const struct eeprom_dev *dev);

/* ============================ 操作 ============================
 * 读 / 写 len 字节，从 addr（相对设备起点，0 起）开始。
 * 返回 EEPROM_OK / EEPROM_ERR_PARAM（空指针 / 句柄无效）/
 *      EEPROM_ERR_RANGE（addr + len > capacity）/ 后端返回的 EEPROM_ERR_IO 等。
 * len == 0 时只要 addr <= capacity 就直接返回 EEPROM_OK，不碰硬件。 */
int eeprom_read(struct eeprom_dev *dev, uint32_t addr, void *buf, size_t len);
int eeprom_write(struct eeprom_dev *dev, uint32_t addr, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_EEPROM_H__ */
