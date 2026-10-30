/*
 * storage_cmd.c - 配置存储的 msh 命令 <modules/msh/>
 *
 * 只通过 eeprom_storage 的 key-value 接口操作配置，不 include 任何驱动头、
 * 不碰总线 / 寄存器，所以这层和具体 EEPROM 型号无关：是哪颗片子、挂在哪条
 * 总线，全由板级 board.c 构造后端并注册成「默认实例」。
 *
 * 命令：
 *   eeprom_get  <key>           读一项（只读缓存，必要时自动从 ROM 加载一次）
 *   eeprom_set  <key> <value>  改一项并立即落盘
 *   eeprom_save                 把缓存写回 ROM（没改动时一个字节都不写）
 *   eeprom_load                 从 ROM 重读，覆盖内存缓存
 *
 * 限制：msh 按空格切参数，所以 value 里不能带空格；要存带空格的值请走 API。
 */
#include <rtthread.h>
#include <stdlib.h>

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

/* ================= eeprom_get ================= */

static void eeprom_get(int argc, char **argv)
{
    eeprom_storage_t *es = eeprom_handle();
    char *v;

    if (es == NULL)
    {
        return;
    }
    if (argc < 2)
    {
        rt_kprintf("用法: eeprom_get <key>\n    例: eeprom_get ssid\n");
        return;
    }

    v = es->get_value(es, argv[1]);
    if (v == NULL)
    {
        rt_kprintf("%s: 未设置（或 ROM 读取失败，可用 eeprom_load 复查）\n", argv[1]);
        return;
    }
    rt_kprintf("%s = %s\n", argv[1], v);
    osal_free(v);
}
MSH_CMD_EXPORT(eeprom_get, eeprom_get <key>: 读一项配置);

/* ================= eeprom_set ================= */

static void eeprom_set(int argc, char **argv)
{
    eeprom_storage_t *es = eeprom_handle();
    int ret;

    if (es == NULL)
    {
        return;
    }
    if (argc < 3)
    {
        rt_kprintf("用法: eeprom_set <key> <value>\n    例: eeprom_set ssid test\n");
        return;
    }

    ret = es->set_value(es, argv[1], argv[2]);
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
    rt_kprintf("%s <= %s（已落盘）\n", argv[1], argv[2]);
}
MSH_CMD_EXPORT(eeprom_set, eeprom_set <key> <value>: 改一项并立即落盘);

/* ================= eeprom_save ================= */

static void eeprom_save(int argc, char **argv)
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
MSH_CMD_EXPORT(eeprom_save, eeprom_save: 把缓存写回 ROM);

/* ================= eeprom_load ================= */

static void eeprom_load(int argc, char **argv)
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
MSH_CMD_EXPORT(eeprom_load, eeprom_load: 从 ROM 重读配置);
