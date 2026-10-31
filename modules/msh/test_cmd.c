/*
 * test_cmd.c - 存储 / NOR FLASH 自测的 msh 命令 <modules/msh/>
 *
 * 原来由 main() 里「按一下 WK_UP」触发，现在改成命令，随时可跑：
 *
 *   test            存储 + FLASH 都测
 *   test storage    只测 EEPROM（key-value 落盘/读回）
 *   test flash      只测 25Q128（找空扇区 → 擦/跨页写/读回 → 擦回空）
 *
 * 两个测试都会真的写片子（EEPROM 一次、FLASH 2 次扇区擦除），所以不自动跑。
 * 代码从 applications/main.c 搬来，判断逻辑没变，只是入口变成 msh 命令。
 */
#include <rtthread.h>
#include <string.h>

#include <osal.h>

#include "eeprom_storage.h"
#include "w25q128.h"

/* ---------------------------------------------------------------------------
 * 存储自测：由 msh 的 `selftest storage` 触发，不建线程
 *
 * 直接用 board_storage_init() 注册好的默认实例，不另造对象：
 *   写一项 → save 落盘 → load 从 ROM 重读 → 比对。
 *
 * 中间那次 load 是必需的：get_value() 只读内存缓存，写完立刻 get 等于自问自答
 * （save 一个字节都没写也照样返回刚写的值）；只有 load 掉缓存再从 EEPROM 读回来，
 * 才能证明「写进片子 + 读出来 + 反序列化」这条链真的通了。
 *
 * 只打印不阻塞：失败也不影响系统继续运行（默认实例是否可用已由
 * board_storage_init 判过）。
 * 值已经是 "ok" 时 set_value 不置脏、save 一个字节都不写，所以反复执行
 * 也不会磨损片子。
 * --------------------------------------------------------------------------- */
#define STORAGE_TEST_KEY     "selftest"
#define STORAGE_TEST_VALUE   "ok"

static void storage_selftest(void)
{
    eeprom_storage_t *es = eeprom_storage_get_default();
    char *v = OSAL_NULL;
    int ret;

    if (es == OSAL_NULL)
    {
        osal_printf("\n[storage-test] SKIP：板级未注册默认实例（EEPROM 未装配）");
        return;
    }

    /* 1) 写缓存 + 立即落盘 */
    ret = es->set_value(es, STORAGE_TEST_KEY, STORAGE_TEST_VALUE);
    if (ret == OSAL_OK)
    {
        ret = es->save(es);
    }

    /* 2) 冲掉缓存、从 EEPROM 重建（跳过这一步，下面的 get 只会命中缓存） */
    if (ret == OSAL_OK)
    {
        ret = es->load(es);
    }
    if (ret != OSAL_OK)
    {
        /* -9 = 写后回读校验不过（器件 ACK 了但没落盘），其余看 eeprom_storage.h */
        osal_printf("\n[storage-test] FAIL：写/读 EEPROM 失败 ret=%d", ret);
        osal_printf("\n  排查：msh 里 `i2c scan` 看 0x50 是否在，"
                    "`i2c read 0x50 0x00 8` 看 ROM 里到底存了什么");
        return;
    }

    /* 3) 比对 */
    v = es->get_value(es, STORAGE_TEST_KEY);
    if (v != OSAL_NULL && strcmp(v, STORAGE_TEST_VALUE) == 0)
    {
        osal_printf("\n[storage-test] PASS：%s = %s（save -> load 读回一致）",
                    STORAGE_TEST_KEY, v);
    }
    else
    {
        /* 走到这里说明 load 认为 ROM 是空的（头部不是本模块格式），
         * 即「写进去了但读不回来 / 根本没写进去」。把查法直接打出来。 */
        osal_printf("\n[storage-test] FAIL：%s 读回 %s，期望 \"%s\"",
                    STORAGE_TEST_KEY,
                    v != OSAL_NULL ? v : "(null)", STORAGE_TEST_VALUE);
        osal_printf("\n  排查：`i2c read 0x50 0x00 8` 看 ROM 首字节"
                    "（45 01=格式对；FF FF=没落盘，多为 WP 拉高/写保护；"
                    "其它=写入错位）");
    }

    osal_free(v);
}

/* ---------------------------------------------------------------------------
 * NOR FLASH 自测：上电跑一次，不建线程
 *
 * 和存储自测不一样：NOR 写之前必须擦一整个 4KB 扇区，随便找块地方测就等于
 * 把那块数据毁了。所以这里的做法是「**只测空扇区，测完还原成空**」：
 *
 *   1) 从片子末尾往前扫最多 FLASH_TEST_SCAN_SECTORS 个扇区，找一个全 0xFF 的；
 *   2) 找到 → 擦除 → 从 +0xF5 写 20 字节（天然跨 256 字节页边界）→ 读回比对
 *      → 再擦回 0xFF。结束时和开始时一样，什么都没留下；
 *   3) 一个空的都没有 → SKIP，并打印「第一个非 0xFF 字节」的位置和值，
 *      不用再手动 flash read 去猜。
 *
 * 为什么要扫而不是死磕最后一扇区：这颗片子可能被厂商例程 / 以前的固件写过，
 * 末尾只剩垃圾数据；扫几个扇区就能找到一个干净的继续测，同时仍然保证
 * 一个字节都不破坏。
 *
 * 只打印，不阻塞。
 * 代价：每执行一次 `selftest flash` 会有 2 次扇区擦除、约 100ms。25Q128 的
 * 擦写寿命是 10 万次，不必频繁跑；它只碰全 0xFF 的空扇区，不会破坏数据。
 * --------------------------------------------------------------------------- */
#define FLASH_TEST_OFFSET       0xF5u   /* 故意不对齐 256 字节页，顺带验证页切分 */
#define FLASH_TEST_LEN          20u
#define FLASH_TEST_SCAN_SECTORS 4u      /* 末尾最多看 4 个扇区（16KB） */

/* 扇区是否全 0xFF。
 * 返回 1 = 空；0 = 有数据，同时给出第一个非 0xFF 字节的偏移和值；负值 = 读失败。 */
static int flash_sector_blank(struct w25q128_dev *dev, uint32_t addr,
                              uint32_t *bad_off, uint8_t *bad_val)
{
    uint8_t buf[64];
    uint32_t off;
    unsigned int i;
    int ret;

    for (off = 0; off < W25Q_SECTOR_SIZE; off += sizeof(buf))
    {
        ret = w25q128_read(dev, addr + off, buf, sizeof(buf));
        if (ret < 0)
        {
            return ret;
        }
        for (i = 0; i < sizeof(buf); i++)
        {
            if (buf[i] != 0xFFu)
            {
                if (bad_off != OSAL_NULL)
                {
                    *bad_off = off + i;
                }
                if (bad_val != OSAL_NULL)
                {
                    *bad_val = buf[i];
                }
                return 0;
            }
        }
    }
    return 1;
}

static void flash_selftest(void)
{
    struct w25q128_dev *dev = w25q128_get_default();
    uint8_t w[FLASH_TEST_LEN];
    uint8_t r[FLASH_TEST_LEN];
    uint32_t addr;
    unsigned int i;
    int ret;

    if (dev == OSAL_NULL)
    {
        osal_printf("\n[flash-test] SKIP：板级未注册 FLASH（25Q128 未装配）");
        return;
    }

    /* 先探测：init 只绑总线、不读 ID（它跑在调度器起来之前，不能碰总线），
     * 所以容量要等这里才有效。不先探测就拿 capacity 算地址会下溢 ——
     * 0x1000000 - 0x1000 变成 0xFFFFF000，读出去直接 W25Q_ERR_RANGE。 */
    ret = w25q128_probe(dev);
    if (ret < 0)
    {
        osal_printf("\n[flash-test] FAIL：25Q128 无应答 ret=%d", ret);
        osal_printf("\n  排查：-7 = JEDEC ID 不对 / 片子没应答；-8 = BUSY 卡住；"
                    "其余看 SPI 总线。可先跑 `flash info`");
        return;
    }

    /* 从末尾往前找一个空扇区来测：自测只碰全 0xFF 的区域，测完擦回空，
     * 所以片子里有没有数据都不影响。 */
    {
        uint32_t bad_off = 0u;
        uint8_t bad_val = 0u;
        int found = 0;

        for (i = 0; i < FLASH_TEST_SCAN_SECTORS; i++)
        {
            uint32_t base = dev->capacity - ((uint32_t)(i + 1u) * W25Q_SECTOR_SIZE);
            uint32_t off = 0u;
            uint8_t val = 0u;

            ret = flash_sector_blank(dev, base, &off, &val);
            if (ret < 0)
            {
                osal_printf("\n[flash-test] FAIL：读扇区 0x%06x 失败 ret=%d",
                            (unsigned int)base, ret);
                osal_printf("\n  排查：-7 = JEDEC ID 不对 / 片子没应答；-8 = BUSY 卡住；"
                            "其余看 SPI 总线。可先跑 `flash info`");
                return;
            }
            if (ret == 1)
            {
                addr = base;
                found = 1;
                break;
            }
            if (i == 0u)
            {
                /* 末尾那个扇区的诊断信息留着，一个空的都找不到时打出来 */
                addr = base;
                bad_off = off;
                bad_val = val;
            }
        }

        if (found == 0)
        {
            uint8_t head[16];
            uint32_t dump = addr + (bad_off & ~0x0Fu);

            osal_printf("\n[flash-test] SKIP：末尾 %u 个扇区都不空，跳过写测试以免破坏数据",
                        (unsigned int)FLASH_TEST_SCAN_SECTORS);
            osal_printf("\n  例：0x%06x 的第一个非 0xFF 字节在偏移 0x%03x = 0x%02x",
                        (unsigned int)addr, (unsigned int)bad_off, bad_val);
            if (w25q128_read(dev, dump, head, sizeof(head)) == 0)
            {
                osal_printf("\n  该处 16 字节：%02x %02x %02x %02x %02x %02x %02x %02x"
                            " %02x %02x %02x %02x %02x %02x %02x %02x",
                            head[0], head[1], head[2], head[3],
                            head[4], head[5], head[6], head[7],
                            head[8], head[9], head[10], head[11],
                            head[12], head[13], head[14], head[15]);
            }
            osal_printf("\n  想强制测可用 `flash test`（它会打 0 号扇区的主意）");
            return;
        }
    }

    /* 1) 擦成空 */
    ret = w25q128_erase_sector(dev, addr);
    if (ret < 0)
    {
        goto fail;
    }

    /* 2) 写一段跨页图样（0xF5 起 20 字节：前 11 字节在页 0，后 9 字节在页 1） */
    for (i = 0; i < sizeof(w); i++)
    {
        w[i] = (uint8_t)(0x5Au ^ (i * 0x11u));
    }
    ret = w25q128_write(dev, addr + FLASH_TEST_OFFSET, w, sizeof(w));
    if (ret < 0)
    {
        goto fail;
    }

    /* 3) 读回比对 */
    for (i = 0; i < sizeof(r); i++)
    {
        r[i] = 0u;
    }
    ret = w25q128_read(dev, addr + FLASH_TEST_OFFSET, r, sizeof(r));
    if (ret < 0)
    {
        goto fail;
    }
    for (i = 0; i < sizeof(w); i++)
    {
        if (r[i] != w[i])
        {
            osal_printf("\n[flash-test] FAIL：偏移 %u 读回 %02x，期望 %02x",
                        i, r[i], w[i]);
            osal_printf("\n  排查：`flash read 0x%06x 32` 看原始字节",
                        (unsigned int)(addr + FLASH_TEST_OFFSET));
            /* 这里不 return：下面还要把扇区擦回空，别给下次上电留脏数据 */
            ret = -1;
            goto restore;
        }
    }

    osal_printf("\n[flash-test] PASS：扇区 0x%06x 擦除/跨页写/读回一致（已擦回空）",
                (unsigned int)addr);

restore:
    /* 4) 擦回 0xFF：自测前后扇区状态一致，不占空间也不影响将来的数据 */
    if (w25q128_erase_sector(dev, addr) < 0)
    {
        osal_printf("\n[flash-test] 警告：扇区 0x%06x 擦回失败，"
                    "下次上电可能显示 SKIP", (unsigned int)addr);
    }
    return;

fail:
    osal_printf("\n[flash-test] FAIL：末扇区 0x%06x 擦/写失败 ret=%d",
                (unsigned int)addr, ret);
    osal_printf("\n  排查：`flash info` 看 ID / `flash test` 手动跑一遍");
}

/* ============================ msh 入口 ============================ */

static int test(int argc, char **argv)
{
    const char *what = (argc > 1) ? argv[1] : "all";

    if ((strcmp(what, "all") == 0) || (strcmp(what, "storage") == 0))
    {
        osal_printf("\n========== 存储自测 ==========");
        storage_selftest();
    }

    if ((strcmp(what, "all") == 0) || (strcmp(what, "flash") == 0))
    {
        osal_printf("\n========== FLASH 自测 ==========");
        flash_selftest();
    }

    if ((strcmp(what, "all") != 0) && (strcmp(what, "storage") != 0) &&
        (strcmp(what, "flash") != 0))
    {
        rt_kprintf("用法: test [storage|flash]\n");
        rt_kprintf("  省略参数 = 存储 + FLASH 都测\n");
        return 0;
    }

    osal_printf("\n[test] 完成\n");
    return 0;
}
MSH_CMD_EXPORT(test, test [storage|flash]: 存储 / FLASH 自测);
