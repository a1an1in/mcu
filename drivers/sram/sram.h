/*
 * sram.h - 外部 SRAM 接口（FSMC 并口 SRAM） <drivers/sram/>
 *
 * 分层：
 *   sram.h              接口，芯片 / OS 无关
 *   sram_config.h       板级事实：Bank、基地址、容量、时序
 *   stm32f1_fsmc_sram.c 芯片实现：FSMC + GPIO
 *
 * FSMC 把外部 SRAM 映射成一段线性地址（战舰 V4 上是 0x68000000 起 1MB），
 * 所以读写就是 memcpy，不需要命令字。
 */
#ifndef DRIVERS_SRAM_H__
#define DRIVERS_SRAM_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SRAM_OK             0
#define SRAM_ERR_PARAM      (-1)
#define SRAM_ERR_NODEV      (-2)
#define SRAM_ERR_VERIFY     (-3)

struct sram_dev {
    uint32_t base;          /* FSMC 映射基地址 */
    uint32_t size;          /* 容量（字节） */
    uint8_t ready;
};

int sram_bind(struct sram_dev *dev, uint32_t base, uint32_t size);

/* 配置 GPIO + FSMC，让 base 处可以像普通内存一样读写 */
int sram_init(struct sram_dev *dev);

/* 抽样写读校验（会破坏一小段内容） */
int sram_selftest(struct sram_dev *dev);

void sram_write(struct sram_dev *dev, uint32_t off, const void *src, uint32_t len);
void sram_read(struct sram_dev *dev, uint32_t off, void *dst, uint32_t len);

void sram_set_default(struct sram_dev *dev);
struct sram_dev *sram_get_default(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_SRAM_H__ */
