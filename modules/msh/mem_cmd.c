/*
 * mem_cmd.c - 堆内存查看 <modules/msh/>
 *
 *   mem    打印堆总量 / 已用 / 峰值 / 剩余
 *
 * 工程没编 RT-Thread 自带的 free 命令，这里直接调 rt_memory_info()。
 * 排查“创建线程失败”时先看这里的剩余量。
 */
#include <rtthread.h>

static int mem_cmd(int argc, char **argv)
{
    rt_size_t total = 0;
    rt_size_t used = 0;
    rt_size_t max_used = 0;

    (void)argc;
    (void)argv;

    rt_memory_info(&total, &used, &max_used);

    if (total == 0u)
    {
        rt_kprintf("堆未启用（配置里没有 RT_USING_HEAP）\n");
        return 0;
    }

    rt_kprintf("堆: 总 %u B (%u KB)，已用 %u B (%u KB)\n",
               (unsigned)total, (unsigned)(total / 1024u),
               (unsigned)used, (unsigned)(used / 1024u));
    rt_kprintf("    峰值 %u B (%u KB)，剩余 %u B (%u KB)\n",
               (unsigned)max_used, (unsigned)(max_used / 1024u),
               (unsigned)(total - used), (unsigned)((total - used) / 1024u));

    return 0;
}

MSH_CMD_EXPORT_ALIAS(mem_cmd, mem, mem: 查看堆内存（总量/已用/峰值/剩余）);
