/*
 * storage_cmd.c - 配置存储的 msh 命令 <modules/msh/>
 *
 * 只通过 eeprom_storage 的 key-value 接口操作配置，不 include 任何驱动头、
 * 不碰总线 / 寄存器，所以这层和具体 EEPROM 型号无关：是哪颗片子、挂在哪条
 * 总线，全由板级 board.c 构造后端并注册成「默认实例」。
 *
 * 命令只导出一个 `eeprom`，其余动作都是子命令：
 *   eeprom get  <key>           读一项（只读缓存，必要时自动从 ROM 加载一次）
 *   eeprom set  <key> <value>   改一项并立即落盘
 *   eeprom save                 把缓存写回 ROM（没改动时一个字节都不写）
 *   eeprom load                 从 ROM 重读，覆盖内存缓存
 *   eeprom help                 打印用法
 *
 * 限制：msh 按空格切参数，所以 value 里不能带空格；要存带空格的值请走 API。
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include <osal.h>

#include "eeprom_storage.h"

/* ================= 公共小工具 ================= */

static eeprom_storage_t *eeprom_handle(void)
{
    eeprom_storage_t *es = eeprom_storage_get_default();

    if (es == NULL)
    {
        rt_kprintf("配置存储未初始化（board 未注册默认实例）\n");
    }
    return es;
}

static const char *eeprom_err_str(int err)
{
    switch (err)
    {
    case OSAL_OK:         return "OK";
    case OSAL_EINVAL:     return "参数非法";
    case OSAL_ENOMEM:     return "内存不足";
    case OSAL_EBUSY:      return "忙";
    case OSAL_EIO:        return "EEPROM 读写失败";
    case ES_ERR_NOSPACE:  return "容量不够";
    case ES_ERR_FORMAT:   return "ROM 数据损坏";
    default:              return "未知错误";
    }
}

static void eeprom_usage(void)
{
    rt_kprintf("用法: eeprom <子命令> [参数]\n"
               "  eeprom get  <key>           读一项（只读缓存，必要时自动从 ROM 加载）\n"
               "  eeprom set  <key> <value>   改一项并立即落盘\n"
               "  eeprom save                 把缓存写回 ROM（没改动时一个字节都不写）\n"
               "  eeprom load                 从 ROM 重读，覆盖内存缓存\n"
               "  例: eeprom set ssid test    eeprom get ssid    eeprom load\n");
}

/* ================= 子命令：get ================= */

static void eeprom_do_get(int argc, char **argv)
{
    eeprom_storage_t *es = eeprom_handle();
    char *v;

    if (es == NULL)
    {
        return;
    }
    if (argc < 1)
    {
        rt_kprintf("用法: eeprom get <key>\n    例: eeprom get ssid\n");
        return;
    }

    v = es->get_value(es, argv[0]);
    if (v == NULL)
    {
        rt_kprintf("%s: 未设置（或 ROM 读取失败，可用 eeprom load 复查）\n", argv[0]);
        return;
    }
    rt_kprintf("%s = %s\n", argv[0], v);
    osal_free(v);
}

/* ================= 子命令：set ================= */

static void eeprom_do_set(int argc, char **argv)
{
    eeprom_storage_t *es = eeprom_handle();
    int ret;

    if (es == NULL)
    {
        return;
    }
    if (argc < 2)
    {
        rt_kprintf("用法: eeprom set <key> <value>\n    例: eeprom set ssid test\n");
        return;
    }

    ret = es->set_value(es, argv[0], argv[1]);
    if (ret != OSAL_OK)
    {
        rt_kprintf("设置失败: %s(%d)\n", eeprom_err_str(ret), ret);
        return;
    }

    ret = es->save(es);
    if (ret != OSAL_OK)
    {
        rt_kprintf("已改缓存但落盘失败: %s(%d)\n", eeprom_err_str(ret), ret);
        return;
    }
    rt_kprintf("%s <= %s（已落盘）\n", argv[0], argv[1]);
}

/* ================= 子命令：save ================= */

static void eeprom_do_save(int argc, char **argv)
{
    eeprom_storage_t *es = eeprom_handle();
    int ret;

    (void)argc;
    (void)argv;

    if (es == NULL)
    {
        return;
    }

    ret = es->save(es);
    if (ret == OSAL_OK)
    {
        rt_kprintf("save: OK\n");
    }
    else
    {
        rt_kprintf("save: %s(%d)\n", eeprom_err_str(ret), ret);
    }
}

/* ================= 子命令：load ================= */

static void eeprom_do_load(int argc, char **argv)
{
    eeprom_storage_t *es = eeprom_handle();
    int ret;

    (void)argc;
    (void)argv;

    if (es == NULL)
    {
        return;
    }

    ret = es->load(es);
    if (ret == OSAL_OK)
    {
        rt_kprintf("load: OK\n");
    }
    else
    {
        rt_kprintf("load: %s(%d)\n", eeprom_err_str(ret), ret);
    }
}

/* ================= 入口：eeprom <子命令> [参数] =================
 * 只导出这一个命令，动作全部走子命令，避免在 msh 里刷出一堆 eeprom_xxx。
 * 子处理函数收到的是「去掉 eeprom 和子命令之后」的参数（argv[0] 即第一个参数）。
 *
 * 命令名 = 函数名 = `eeprom`（MSH_CMD_EXPORT 拿符号名当命令名）。
 * 这里不叫 eeprom_cmd：msh 里敲的是 `eeprom set ssid test`，
 * 不是 `eeprom_cmd ...`；文件名保留 _cmd 后缀只是表示「这是一个 msh 适配层」。
 */
static void eeprom(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        eeprom_usage();
        return;
    }

    sub = argv[1];
    if (strcmp(sub, "get") == 0)
    {
        eeprom_do_get(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "set") == 0)
    {
        eeprom_do_set(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "save") == 0)
    {
        eeprom_do_save(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "load") == 0)
    {
        eeprom_do_load(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "help") == 0 || strcmp(sub, "?") == 0)
    {
        eeprom_usage();
    }
    else
    {
        rt_kprintf("未知子命令: %s\n", sub);
        eeprom_usage();
    }
}
MSH_CMD_EXPORT(eeprom, eeprom <get|set|save|load>: EEPROM 配置读写);
