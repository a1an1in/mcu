/*
 * voice_cmd.c - 语音对话的 msh 命令 + 服务自启动 <modules/msh/>
 *
 *   voice key [on|off]   打开/关闭 KEY0 语音键（默认打开）
 *   voice info           查看状态 / 网关
 *
 * 语音流程：按住 KEY0 说话，松开后自动 ASR → 大模型 → TTS → 播放。
 */
#include <rtthread.h>
#include <string.h>

#include "ai.h"
#include "voice.h"

static void voice_usage(void)
{
    rt_kprintf("用法: voice <key|info>\n");
    rt_kprintf("  voice key [on|off]  打开/关闭 KEY0 语音键（默认 on）\n");
    rt_kprintf("  voice info          查看语音键 / 网关状态\n");
    rt_kprintf("  用法: 按住 KEY0 说话，松开后自动识别、回答并播放\n");
}

static int voice(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "info";

    if (strcmp(sub, "key") == 0)
    {
        if (argc > 2)
        {
            if (strcmp(argv[2], "on") == 0)
            {
                voice_key_enable(1);
            }
            else if (strcmp(argv[2], "off") == 0)
            {
                voice_key_enable(0);
            }
            else
            {
                rt_kprintf("用法: voice key [on|off]\n");
                return 0;
            }
        }
        rt_kprintf("KEY0 语音键: %s\n", voice_key_enabled() ? "on" : "off");
        return 0;
    }

    if (strcmp(sub, "info") == 0)
    {
        char ip[24];
        uint16_t port = 0;

        rt_kprintf("语音键: %s\n", voice_key_enabled() ? "on（按住 KEY0 说话）" : "off");
        rt_kprintf("状态: %s%s\n",
                   voice_is_busy() ? "处理中" : "空闲",
                   voice_is_recording() ? "（录音中）" : "");
        if (ai_get_server(ip, sizeof(ip), &port) == AI_OK)
        {
            rt_kprintf("网关: %s:%u\n", ip, (unsigned)port);
        }
        else
        {
            rt_kprintf("网关: 未设置（先 ai server <PC_IP> [port]）\n");
        }
        return 0;
    }

    if ((strcmp(sub, "help") == 0) || (strcmp(sub, "?") == 0))
    {
        voice_usage();
        return 0;
    }

    rt_kprintf("未知子命令: %s\n", sub);
    voice_usage();
    return 0;
}
MSH_CMD_EXPORT(voice, voice <key|info>: 按住 KEY0 语音对话);

/* 系统初始化（main 之前、调度器已运行）自动启动按键监听线程 */
static int voice_autostart(void)
{
    (void)voice_service_start();
    return 0;
}
INIT_APP_EXPORT(voice_autostart);
