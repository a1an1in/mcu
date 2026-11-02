/*
 * sram_pool.c - 外部 SRAM 上的 first-fit 分配器 <modules/sram/>
 *
 * 只在启动时初始化一次：整段 SRAM 作为一个大空闲块，块头 8 字节。
 * 分配/释放都在 SRAM 里原地操作，不占内部 RAM。
 */
#include "sram.h"
#include "sram_pool.h"

#define BLK_HDR         (sizeof(struct blk))
#define ALIGN8(x)       (((x) + 7u) & ~7u)

struct blk {
    uint32_t size;      /* 数据区大小（不含块头） */
    uint32_t free;      /* 1 = 空闲 */
};

static struct sram_dev *s_dev;

int sram_pool_init(void)
{
    struct sram_dev *dev = sram_get_default();
    struct blk *b;

    if ((dev == NULL) || (dev->ready == 0u) || (dev->size < 4096u))
    {
        return -1;
    }

    b = (struct blk *)dev->base;
    b->size = dev->size - BLK_HDR;
    b->free = 1u;
    s_dev = dev;
    return 0;
}

void *sram_alloc(size_t size)
{
    uint8_t *p;
    uint8_t *end;
    uint32_t need;

    if ((s_dev == NULL) && (sram_pool_init() != 0))
    {
        return NULL;
    }
    if (size == 0u)
    {
        return NULL;
    }

    need = ALIGN8(size);
    p = (uint8_t *)s_dev->base;
    end = p + s_dev->size;

    while ((p + BLK_HDR) <= end)
    {
        struct blk *b = (struct blk *)p;

        if ((b->free != 0u) && (b->size >= need))
        {
            if (b->size >= (need + BLK_HDR + 8u))
            {
                struct blk *n = (struct blk *)(p + BLK_HDR + need);

                n->size = b->size - need - BLK_HDR;
                n->free = 1u;
                b->size = need;
            }
            b->free = 0u;
            return p + BLK_HDR;
        }
        p += BLK_HDR + b->size;
    }
    return NULL;
}

void sram_free(void *ptr)
{
    struct blk *b;
    uint8_t *p;
    uint8_t *end;

    if ((ptr == NULL) || (s_dev == NULL))
    {
        return;
    }

    /* 基本合法性检查，避免野指针把块表写坏 */
    if (((uint8_t *)ptr < (uint8_t *)s_dev->base + BLK_HDR) ||
        ((uint8_t *)ptr >= (uint8_t *)s_dev->base + s_dev->size) ||
        ((((uint32_t)(uintptr_t)ptr) & 7u) != 0u))
    {
        return;
    }

    b = (struct blk *)((uint8_t *)ptr - BLK_HDR);
    b->free = 1u;

    /* 从头扫一遍，合并相邻空闲块 */
    p = (uint8_t *)s_dev->base;
    end = p + s_dev->size;
    while ((p + BLK_HDR) <= end)
    {
        struct blk *c = (struct blk *)p;
        uint8_t *next = p + BLK_HDR + c->size;
        uint8_t *next_end = next + BLK_HDR;

        if ((c->free != 0u) && (next_end <= end))
        {
            struct blk *n = (struct blk *)next;

            if (n->free != 0u)
            {
                c->size += BLK_HDR + n->size;
                continue;
            }
        }
        p = next;
    }
}

void sram_pool_info(uint32_t *total, uint32_t *used,
                    uint32_t *free_bytes, uint32_t *max_free)
{
    uint8_t *p;
    uint8_t *end;
    uint32_t u = 0u;
    uint32_t f = 0u;
    uint32_t m = 0u;

    if (total != NULL) *total = 0u;
    if (used != NULL) *used = 0u;
    if (free_bytes != NULL) *free_bytes = 0u;
    if (max_free != NULL) *max_free = 0u;

    if ((s_dev == NULL) && (sram_pool_init() != 0))
    {
        return;
    }

    p = (uint8_t *)s_dev->base;
    end = p + s_dev->size;

    while ((p + BLK_HDR) <= end)
    {
        struct blk *b = (struct blk *)p;

        if (b->free != 0u)
        {
            f += b->size;
            if (b->size > m)
            {
                m = b->size;
            }
        }
        else
        {
            u += b->size;
        }
        p += BLK_HDR + b->size;
    }

    if (total != NULL) *total = s_dev->size;
    if (used != NULL) *used = u;
    if (free_bytes != NULL) *free_bytes = f;
    if (max_free != NULL) *max_free = m;
}
