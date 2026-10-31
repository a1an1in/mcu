/*
 * main.c - 应用主程序（RT-Thread 主线程入口）—— Hello World 验证版
 *
 * 临时用作烧录验证：仅通过 OSAL 打印 Hello, world!，
 * 验证 osal_printf → rt_kprintf → UART1 控制台这套输出链路。
 * 只依赖 OSAL，不直接接触 RT-Thread API，与具体 RTOS 解耦。
 */
#include <osal.h>

#include "key.h"
#include "led.h"

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

/* ---------------------------------------------------------------------------
 * 硬件自测：板载按键 + 板载 LED（战舰 STM32F103 V4）
 *
 *   KEY0(PE4) 按住 → DS0(PB5) 亮          松开 → 灭
 *   KEY1(PE3) 按住 → DS1(PE5) 亮          松开 → 灭
 *   KEY2(PE2) 按一下 → 两个 LED 每 500ms 交替闪；再按一下 → 停止并全灭
 *
 * 链路：modules/key（20ms 消抖、内部上/下拉、按下边沿事件）
 *        └──> 本线程（映射策略）──> modules/led（低电平点亮）
 * 全程只经过 OSAL 与 gpio.h，换 RTOS（rt-thread/zephyr）或换 GPIO 后端
 * （-DGPIO_BACKEND=）都不用改这里。
 * 按键事件同步打印到 UART1，串口助手上能直接看到。
 *
 * 优先级约定：手动优先 —— 只要 KEY0/KEY1 有一个被按住，就按「按住哪个亮哪个」
 * 显示；两个都松开后，若交替闪烁是开着的，再从当前相位继续。
 * --------------------------------------------------------------------------- */
#define HW_TEST_STACK_SIZE   1024u
#define HW_TEST_PRIORITY     12u
#define HW_TEST_POLL_MS      10u
#define HW_TEST_ALT_TICKS    50u     /* 50 × 10ms = 500ms 交替一次 */

static void key_led_test_entry(void *param)
{
    uint32_t tick = 0;
    int alt_on    = 0;    /* KEY2 切换的「交替闪烁」开关 */
    int alt_phase = 0;    /* 交替相位：0 → DS0 亮，1 → DS1 亮 */
    int last_k0   = -1;   /* -1：确保首轮先把初始状态打印出来 */
    int last_k1   = -1;
    int k0, k1;

    (void)param;

    for (;;)
    {
        k0 = key_is_down(KEY_0);
        k1 = key_is_down(KEY_1);

        /* KEY2 用「读取即清除」的按下边沿：按一下翻转一次，按住不会连翻 */
        if (key_pressed(KEY_2))
        {
            alt_on    = !alt_on;
            alt_phase = 0;
            osal_printf("\n[hw-test] KEY2 按下 -> 交替闪烁 %s",
                        alt_on ? "开始（DS0/DS1 每 500ms 轮流亮）" : "停止（全灭）");
        }

        /* 自由运行的分频器：不论当前什么模式都在走，交替相位因此是连续的 */
        if (++tick >= HW_TEST_ALT_TICKS)
        {
            tick = 0;
            alt_phase = !alt_phase;
        }

        if (k0 || k1)
        {
            /* 手动优先：按住哪个亮哪个（两个都按住就两个都亮） */
            led_set(LED_DS0, k0);
            led_set(LED_DS1, k1);
        }
        else if (alt_on)
        {
            led_set(LED_DS0, alt_phase);
            led_set(LED_DS1, !alt_phase);
        }
        else
        {
            led_set(LED_DS0, 0);
            led_set(LED_DS1, 0);
        }

        /* 只在电平真的变化时打印，避免每 10ms 刷屏 */
        if (k0 != last_k0)
        {
            last_k0 = k0;
            osal_printf("\n[hw-test] KEY0 %s -> DS0 %s",
                        k0 ? "按下" : "松开",
                        k0 ? "点亮" : "熄灭");
        }
        if (k1 != last_k1)
        {
            last_k1 = k1;
            osal_printf("\n[hw-test] KEY1 %s -> DS1 %s",
                        k1 ? "按下" : "松开",
                        k1 ? "点亮" : "熄灭");
        }

        osal_thread_sleep_ms(HW_TEST_POLL_MS);
    }
}

static void hw_test_start(void)
{
    osal_thread_t t = OSAL_NULL;

    led_init();

    key_init();

    led_set(LED_DS0, 0);    /* 明确熄一次，避免上电残留 */
    led_set(LED_DS1, 0);
    osal_printf("\n[hw-test] ready: KEY0 按住->DS0 亮 / KEY1 按住->DS1 亮 /"
                " KEY2 按一下->两灯交替闪（DS0=PB5、DS1=PE5，低电平点亮）");

    if (osal_thread_create(&t, "keyled", key_led_test_entry, OSAL_NULL,
                           OSAL_NULL, HW_TEST_STACK_SIZE,
                           HW_TEST_PRIORITY) != OSAL_OK)
    {
        osal_printf("\n[hw-test] create thread failed");
    }
}

/* RT-Thread 系统完成初始化后，会在 main 线程中调用 main() */
int main(void)
{
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

    /* 硬件自测：KEY0/KEY1 按住点亮各自 LED，KEY2 切换两灯交替闪
     * （起独立线程，主线程继续跑下面的心跳） */
    hw_test_start();

    /* 心跳：主线程不再做按键触发，存储 / FLASH 自测改由 msh 的 test 命令执行。 */
    for (;;)
    {
        osal_thread_sleep_ms(3000u);
    }

    return 0;
}