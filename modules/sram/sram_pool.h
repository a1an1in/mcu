/*
 * sram_pool.h - 外部 SRAM 堆 <modules/sram/>
 *
 * 在 FSMC 映射的那段内存上做一个简单的 first-fit 分配器，
 * 给需要大块缓冲的业务用（比如音频、文件缓存）。
 *
 *   void *p = sram_alloc(64 * 1024);
 *   ...
 *   sram_free(p);
 */
#ifndef MODULES_SRAM_SRAM_POOL_H__
#define MODULES_SRAM_SRAM_POOL_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int sram_pool_init(void);
void *sram_alloc(size_t size);
void sram_free(void *ptr);

/* 统计：总量 / 已用 / 空闲 / 最大空闲块（字节） */
void sram_pool_info(uint32_t *total, uint32_t *used,
                    uint32_t *free_bytes, uint32_t *max_free);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_SRAM_SRAM_POOL_H__ */
