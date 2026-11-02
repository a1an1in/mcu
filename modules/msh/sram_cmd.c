/*
 * sram_cmd.c - 外部 SRAM 命令 <modules/msh/>
 *
 *   sram info           总量 / 已用 / 空闲
 *   sram test           读写自检（会清空 SRAM 内容）
 *   sram alloc <bytes>  分配一块（调 sram_alloc）
 *   sram free <addr>    释放（地址用 16 进制，如 0x68000008）
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "sram.h"
#include "sram_pool.h"

static void sram_usage(void)
{
    rt_kprintf("用法: sram <info|test|alloc <bytes>|free <addr>>\n");
    rt_kprintf("  sram info           总量 / 已用 / 空闲 / 最大空闲块\n");
    rt_kprintf("  sram test           读写自检（会破坏 SRAM 内容）\n");
    rt_kprintf("  sram alloc <bytes>  从外部 SRAM 分配\n");
    rt_kprintf("  sram free <addr>    释放（16 进制地址）\n");
}

static int sram_cmd(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "info";
    struct sram_dev *dev = sram_get_default();

    if (strcmp(sub, "test") == 0)
    {
        int ret;

        if ((dev == NULL) || (dev->ready == 0u))
        {
            rt_kprintf("SRAM 未初始化\n");
            return 0;
        }

        rt_kprintf("自检中（会清空 SRAM 内容）...\n");
        ret = sram_selftest(dev);
        if (ret == SRAM_OK)
        {
            rt_kprintf("SRAM 自检通过：%u KB @ 0x%08x\n",
                       (unsigned)(dev->size / 1024u), (unsigned)dev->base);
            (void)sram_pool_init();     /* 自检破坏了分配表，重新初始化 */
        }
        else
        {
            rt_kprintf("SRAM 自检失败（%d），检查焊接/FSMC 引脚\n", ret);
        }
        return 0;
    }

    if (strcmp(sub, "info") == 0)
    {
        uint32_t total = 0u, used = 0u, free_bytes = 0u, max_free = 0u;

        if ((dev == NULL) || (dev->ready == 0u))
        {
            rt_kprintf("SRAM 未初始化\n");
            return 0;
        }

        sram_pool_info(&total, &used, &free_bytes, &max_free);
        rt_kprintf("SRAM: 总 %u KB，已用 %u KB，空闲 %u KB（最大块 %u KB）\n",
                   (unsigned)(total / 1024u), (unsigned)(used / 1024u),
                   (unsigned)(free_bytes / 1024u), (unsigned)(max_free / 1024u));
        return 0;
    }

    if (strcmp(sub, "alloc") == 0)
    {
        uint32_t n = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0u;
        void *p;

        if (n == 0u)
        {
            sram_usage();
            return 0;
        }

        p = sram_alloc(n);
        if (p == NULL)
        {
            rt_kprintf("分配 %u 字节失败\n", (unsigned)n);
            return 0;
        }
        rt_kprintf("分配 %u 字节：0x%08x\n",
                   (unsigned)n, (unsigned)(uintptr_t)p);
        return 0;
    }

    if (strcmp(sub, "free") == 0)
    {
        void *p;

        if (argc <= 2)
        {
            sram_usage();
            return 0;
        }

        p = (void *)(uintptr_t)strtoul(argv[2], NULL, 16);
        sram_free(p);
        rt_kprintf("已释放 0x%08x\n", (unsigned)(uintptr_t)p);
        return 0;
    }

    sram_usage();
    return 0;
}

MSH_CMD_EXPORT_ALIAS(sram_cmd, sram,
                     sram <info|test|alloc <bytes>|free <addr>>: 外部 SRAM);
