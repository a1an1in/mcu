/*
 * rtthread_i2c.c - I2C 的 RT-Thread 适配 <drivers/i2c/>
 *
 * 只做一件 OS 相关的事：用互斥量实现 i2c_bus_lock / i2c_bus_unlock，
 * 覆盖 i2c_core.c 里的 weak 空实现（链接时强符号优先）。
 *
 * msh 调试命令不在这里 —— 按本工程的分层，「给 shell 用的适配」统一放在
 * modules/msh/（同 storage_cmd.c），见 modules/msh/i2c_cmd.c 的 `i2c` 命令：
 *     i2c scan / i2c read / i2c write / i2c smbtest
 *
 * 互斥量惰性创建：首次加锁时创建，避免 BSP 再额外调用一次初始化函数。
 */
#include <rtthread.h>

#include "i2c.h"

static struct rt_mutex *s_i2c_mutex = RT_NULL;

/* 单位：系统节拍。1s 足够覆盖一次最长传输（EEPROM 页写也就几 ms） */
#define I2C_LOCK_TIMEOUT_MS     1000

int i2c_bus_lock(struct i2c_adapter *adap)
{
    (void)adap;

    if (s_i2c_mutex == RT_NULL)
    {
        s_i2c_mutex = rt_mutex_create("i2cbus", RT_IPC_FLAG_PRIO);
        if (s_i2c_mutex == RT_NULL)
        {
            return I2C_ERR_IO;          /* 堆不够，退化为不保护 */
        }
    }

    if (rt_mutex_take(s_i2c_mutex, rt_tick_from_millisecond(I2C_LOCK_TIMEOUT_MS)) != RT_EOK)
    {
        return I2C_ERR_TIMEOUT;
    }
    return 0;
}

void i2c_bus_unlock(struct i2c_adapter *adap)
{
    (void)adap;

    if (s_i2c_mutex != RT_NULL)
    {
        (void)rt_mutex_release(s_i2c_mutex);
    }
}
