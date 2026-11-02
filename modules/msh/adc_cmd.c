/*
 * adc_cmd.c - ADC 采样命令 <modules/msh/>
 *
 *   adc start [hz]   启动：定时器按 hz 触发，DMA 循环采样（默认 1000Hz）
 *   adc stop         停止
 *   adc read         读一次内部温度
 *   adc info         查看状态 / 采样率 / 原始值
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "sensor.h"
#include "adc.h"

static void adc_usage(void)
{
    rt_kprintf("用法: adc <start [hz]|stop|read|info>\n");
    rt_kprintf("  adc start [hz]  定时器触发 + DMA 采样（默认 1000Hz，32 点循环）\n");
    rt_kprintf("  adc stop        停止采样\n");
    rt_kprintf("  adc read        读一次温度\n");
    rt_kprintf("  adc watch [秒] [ms]  连续打印温度（默认 10 秒、每 1000ms 一次）\n");
    rt_kprintf("  adc cal <当前温度>   单点校准（例: adc cal 25）\n");
    rt_kprintf("  adc info        状态 / 采样率 / 平均原始值\n");
    rt_kprintf("  adc regs        打印关键寄存器（排查一直为 0）\n");
}

static void adc_read_once(void)
{
    int t10;
    uint16_t raw;

    if (sensor_read_temp_x10(&t10) != 0)
    {
        rt_kprintf("还没开始采样，先 adc start\n");
        return;
    }

    (void)sensor_read_raw(&raw);
    rt_kprintf("温度: %d.%d °C（原始 %u，%u Hz，32 点平均）\n",
               t10 / 10, (t10 < 0 ? -t10 : t10) % 10,
               (unsigned)raw, (unsigned)sensor_rate());
}

static int adc_cmd(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "read";

    if (strcmp(sub, "start") == 0)
    {
        uint32_t hz = (argc > 2) ? (uint32_t)atoi(argv[2]) : 0u;

        if (sensor_start(hz) != 0)
        {
            rt_kprintf("启动失败（采样率超出范围，或 ADC 未绑定）\n");
            return 0;
        }
        rt_kprintf("ADC 已启动: %u Hz，32 点循环 DMA\n",
                   (unsigned)sensor_rate());
        return 0;
    }

    if (strcmp(sub, "stop") == 0)
    {
        (void)sensor_stop();
        rt_kprintf("ADC 已停止\n");
        return 0;
    }

    if (strcmp(sub, "watch") == 0)
    {
        uint32_t secs = (argc > 2) ? (uint32_t)atoi(argv[2]) : 10u;
        uint32_t period = (argc > 3) ? (uint32_t)atoi(argv[3]) : 1000u;
        uint32_t n;
        uint32_t i;

        if (secs == 0u)
        {
            secs = 10u;
        }
        if (period < 50u)
        {
            period = 50u;
        }

        if (sensor_is_running() == 0)
        {
            if (sensor_start(1000u) != 0)
            {
                rt_kprintf("启动采样失败\n");
                return 0;
            }
            rt_kprintf("（已自动启动 1000 Hz 采样）\n");
        }

        n = (secs * 1000u) / period;
        for (i = 0u; i < n; i++)
        {
            int t10 = 0;
            uint16_t raw = 0u;
            uint32_t ms = i * period;

            if (sensor_read_temp_x10(&t10) == 0)
            {
                (void)sensor_read_raw(&raw);
                rt_kprintf("  %4u.%01u s  %d.%d °C  raw=%u\n",
                           ms / 1000u, (ms % 1000u) / 100u,
                           t10 / 10, (t10 < 0 ? -t10 : t10) % 10,
                           (unsigned)raw);
            }
            else
            {
                rt_kprintf("  读取失败\n");
                break;
            }
            rt_thread_mdelay(period);
        }
        return 0;
    }

    if (strcmp(sub, "regs") == 0)
    {
        struct adc_regs r;

        adc_debug_regs(&r);
        rt_kprintf("RCC  CFGR=%08x APB2ENR=%08x APB1ENR=%08x AHBENR=%08x\n",
                   (unsigned)r.rcc_cfgr, (unsigned)r.rcc_apb2enr,
                   (unsigned)r.rcc_apb1enr, (unsigned)r.rcc_ahbenr);
        rt_kprintf("ADC1 CR1=%08x CR2=%08x SR=%08x\n",
                   (unsigned)r.adc_cr1, (unsigned)r.adc_cr2, (unsigned)r.adc_sr);
        rt_kprintf("     SQR1=%08x SQR3=%08x SMPR2=%08x\n",
                   (unsigned)r.adc_sqr1, (unsigned)r.adc_sqr3,
                   (unsigned)r.adc_smpr2);
        rt_kprintf("TIM3 CR1=%08x CR2=%08x CNT=%u PSC=%u ARR=%u\n",
                   (unsigned)r.tim_cr1, (unsigned)r.tim_cr2, (unsigned)r.tim_cnt,
                   (unsigned)r.tim_psc, (unsigned)r.tim_arr);
        rt_kprintf("DMA1 CCR=%08x CNDTR=%u CPAR=%08x CMAR=%08x\n",
                   (unsigned)r.dma_ccr, (unsigned)r.dma_cndtr,
                   (unsigned)r.dma_cpar, (unsigned)r.dma_cmar);
        rt_kprintf("DR=%08x  buf[0..3]=%u %u %u %u\n",
                   (unsigned)r.adc_dr, (unsigned)r.sample[0],
                   (unsigned)r.sample[1], (unsigned)r.sample[2],
                   (unsigned)r.sample[3]);
        return 0;
    }

    if (strcmp(sub, "cal") == 0)
    {
        int t10 = 0;

        if (argc <= 2)
        {
            rt_kprintf("用法: adc cal <当前温度，整数°C>，例: adc cal 25\n");
            return 0;
        }
        if (sensor_is_running() == 0)
        {
            rt_kprintf("先 adc start\n");
            return 0;
        }
        if (sensor_calibrate(atoi(argv[2]) * 10) != 0)
        {
            rt_kprintf("校准失败\n");
            return 0;
        }
        (void)sensor_read_temp_x10(&t10);
        rt_kprintf("校准完成，偏移 %+d.%d °C，当前显示 %d.%d °C\n",
                   sensor_offset_x10() / 10,
                   (sensor_offset_x10() < 0 ? -sensor_offset_x10() : sensor_offset_x10()) % 10,
                   t10 / 10, (t10 < 0 ? -t10 : t10) % 10);
        return 0;
    }

    if (strcmp(sub, "info") == 0)
    {
        if (sensor_is_running() == 0)
        {
            rt_kprintf("ADC: 未启动\n");
        }
        else
        {
            uint16_t raw = 0u;

            (void)sensor_read_raw(&raw);
            rt_kprintf("ADC: 运行中，%u Hz，32 点循环，平均原始值 %u\n",
                       (unsigned)sensor_rate(), (unsigned)raw);
        }
        return 0;
    }

    if (strcmp(sub, "read") == 0)
    {
        adc_read_once();
        return 0;
    }

    adc_usage();
    return 0;
}

MSH_CMD_EXPORT_ALIAS(adc_cmd, adc, adc <start [hz]|stop|read|info>: 定时器触发 ADC + DMA);
