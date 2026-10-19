/*
 * main.c - 应用主程序（RT-Thread 主线程入口）—— Hello World 验证版
 *
 * 临时用作烧录验证：仅通过 OSAL 打印 Hello, world!，
 * 验证 osal_printf → rt_kprintf → UART1 控制台这套输出链路。
 * 只依赖 OSAL，不直接接触 RT-Thread API，与具体 RTOS 解耦。
 */
#include <osal.h>

#ifdef __ZEPHYR__
/* Zephyr 专属：运行时枚举已注册的设备/驱动（device 表来自 DTS + 驱动声明）。
   放在共享 main.c 的 __ZEPHYR__ 分支内，RT-Thread 编译完全不经过这里。 */
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <string.h>

/*
 * 通过 STRUCT_SECTION_FOREACH(device, ...) 遍历链接器里的静态设备节，
 * 逐个用 printk(osal_printf) 输出，模拟内核日志行：
 *   [00000 ms] <inf> boot_devices: ...
 *
 * 为什么不用 z_device_get_all_static()：
 *   该函数内部 STRUCT_SECTION_GET(device, 0, devs) 的第二个参数是“只取第 0 个”
 *   的下标，它只把第 0 个设备指针写进数组首元素，其余保持 NULL；返回的却仍是
 *   节总数(cnt)。于是“get_all”拿到 n=11 却只有 devs[0] 有效——这正是此前
 *   只打印 #0、其余全被空指针 continue 跳过的根因。
 *   STRUCT_SECTION_FOREACH 逐个拿到真实设备指针，与 Zephyr 内核用法一致。
 * 每个设备：序号、是否暴露驱动 API、初始化结果、设备名、初始化返回码。
 */
static void boot_devices_dump(const char *rtos)
{
    const struct device *d;
    size_t i = 0u;
    const char *lm;

    (void)rtos; /* 全量信息已含 __ZEPHYR__ 构建标志；rtos 留作将来区分 */

#if defined(CONFIG_LOG_MODE_IMMEDIATE)
    lm = "immediate";
#elif defined(CONFIG_LOG_MODE_DEFERRED)
    lm = "deferred";
#elif defined(CONFIG_LOG_MODE_MINIMAL)
    lm = "minimal";
#else
    lm = "off";
#endif

    /* 内核启动过程日志：头(版本/LOG模式/级别) + 按初始化顺序的设备清单 */
    osal_printf("\n[%u ms] <inf> boot: === Zephyr 内核/驱动启动过程日志 ===",
                (unsigned)k_uptime_get());
    osal_printf("\n[%u ms] <inf> boot:   log_mode=%s  default_log_level=%u  delayed_boot=%u ms",
                (unsigned)k_uptime_get(), lm,
                (unsigned)CONFIG_LOG_DEFAULT_LEVEL, (unsigned)CONFIG_BOOT_DELAY);
    osal_printf("\n[%u ms] <inf> boot_devices: 系统静态设备表(SYS_INIT 顺序)如下：",
                (unsigned)k_uptime_get());

    STRUCT_SECTION_FOREACH(device, d) {
        char nm[40];
        const char *st;

        if (d == NULL) {
            continue;
        }
        if ((d->state != NULL) && d->state->initialized) {
            st = (d->state->init_res == 0u) ? "init-ok" : "init-fail";
        } else {
            st = "pending";
        }
        (void)strncpy(nm, (d->name != NULL) ? d->name : "(anon)", sizeof(nm) - 1u);
        nm[sizeof(nm) - 1u] = '\0';

        osal_printf("\n[%u ms] <inf> boot_devices:   #%-2u  api=%s %-9s  device=%-28s init_res=%u",
                    (unsigned)k_uptime_get(), (unsigned)i,
                    d->api != NULL ? "Y" : "-", st, nm,
                    (d->state != NULL) ? (unsigned)(d->state->init_res) : 0xffu);

        i++;
        /* 每条间隔：保证上一条已完整发出 UART，串口助手必能逐条跟上 */
        k_sleep(K_MSEC(80));
    }

    osal_printf("\n[%u ms] <inf> boot_devices: ---- 驱动清单结束，共 %u 条 ----",
                (unsigned)k_uptime_get(), (unsigned)i);
}
#else  /* !__ZEPHYR__  → RT-Thread */

/*
 * 启动过程日志（RT-Thread）：
 *   - RT-Thread 内核启动时本身会打印启动 logo + 版本号（内核自带，非本函数）；
 *   - 本裁剪版未启用 RT_USING_DEVICE（无设备框架），无法枚举"注册设备"，
 *     因此这里改为打印内核启动时"已创建的线程"清单——更切合 RT-Thread 的启动过程，
 *     走 RT-Thread 内核新增的只读访问器 rt_startup_threads_dump()
 *     （由 patches/ 补丁应用到 rtos/rt-thread/src/object.c，与 Zephyr 那次效果对齐）。
 *   extern 声明仅在 RT-Thread 编译分支内，避免污染 Zephyr。
 */
static void boot_devices_dump_rtthread(const char *rtos)
{
    extern void rt_startup_threads_dump(void);

    osal_printf("\n[%s] RT-Thread 内核启动过程（RTOS=%s）：", rtos, rtos);
    osal_printf("\n  注：本裁剪版未启用 RT_USING_DEVICE（无设备框架），");
    osal_printf("无法枚举注册设备；下面打印内核启动时已创建的线程：");
    rt_startup_threads_dump(); /* 该函数内部走 rt_kprintf 同步输出 */
}

#endif /* __ZEPHYR__ */

/* RT-Thread 系统完成初始化后，会在 main 线程中调用 main() */
int main(void)
{
    uint32_t n = 0;

    /* 编译期判定当前 RTOS 后端（两个平台复用同一份 main.c）：
     *   - Zephyr 构建总会用编译器命令行传 -D__ZEPHYR__=1，因此对它专门判断；
     *   - 其余即 RT-Thread（本工程仅支持 zephyr / rt-thread 两个后端）。
     * 这样 USB/串口日志里的系统名即为“真实运行的系统”，可直接区分。 */
#if defined(__ZEPHYR__)
    const char *rtos = "zephyr";
#else
    const char *rtos = "rt-thread";
#endif

    /* Zephyr 构建：启动时一次性打印已加载的驱动清单（RT-Thread 不执行此分支） */
#ifdef __ZEPHYR__
    boot_devices_dump(rtos);
#else
    boot_devices_dump_rtthread(rtos);
#endif

    /* 周期性打印：把 Hello 也并入每 3 秒一次的心跳，避免因串口助手
     * 开启较晚/首屏刷新而错过启动时的一次性输出，保证任何时候都能看到。 */
    for (;;)
    {
        osal_printf("\n[app30-%s] heartbeat #%u : Hello, world! (STM32F103, OSAL backend: %s) via UART1",
                    rtos, (unsigned)n++, rtos);
        osal_thread_sleep_ms(3000u);
    }

    return 0;
}