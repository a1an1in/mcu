/*
 * chat_cmd.c - 文本 / 语音对话的统一 msh 命令 <modules/msh/>
 *
 *   chat text <text...>      文本对话（HTTP /ask）
 *   chat voice [on|off]      KEY0 语音键开关 / 状态
 *   chat voice vol [0-100]   语音播放音量（和 mplayer vol 共用）
 *   chat voice gain [0-15]   MIC 增益（0 = 自动 AGC）
 *   chat server <ip> [port]  设置 PC 网关（默认 8765）
 *   chat info                网关 / 文本 / 语音状态
 *
 * 文本和语音都通过 PC 上的 tools/llm_bridge.py：
 *   /ask   纯文本 JSON → 文本
 *   /voice 流式 PCM → ASR → LLM → TTS → 音频
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "music.h"
#include "voice.h"

static const char *ai_err_str(int ret)
{
    switch (ret)
    {
    case AI_OK:             return "OK";
    case AI_ERR_PARAM:      return "参数非法（IP / 端口 / 问题为空）";
    case AI_ERR_NOSERVER:   return "还没设置网关，先 chat server <PC_IP> [port]";
    case AI_ERR_NET:        return "连不上 PC 网关（IP / 防火墙 / bridge 是否在跑？）";
    case AI_ERR_TOOBIG:     return "问题太长，缩短一点再试";
    default:                return "未知错误";
    }
}

static void chat_usage(void)
{
    rt_kprintf("用法: chat <text|voice|server|info>\n");
    rt_kprintf("  chat text <text...>        文本对话，例: chat text 你好\n");
    rt_kprintf("  chat voice [on|off]        打开/关闭 KEY0 语音键（默认 on）\n");
    rt_kprintf("  chat voice vol [0-100]     查询/设置播放音量（和 mplayer vol 共用）\n");
    rt_kprintf("  chat voice gain [0-15]     查询/设置 MIC 增益（0=自动 AGC，默认 4）\n");
    rt_kprintf("  chat server <ip> [port]    设置 PC 网关（默认 8765）\n");
    rt_kprintf("  chat info                  网关 / 文本 / 语音状态\n");
    rt_kprintf("  语音用法: 按住 KEY0 说话，松开后自动识别、回答并播放\n");
}

static void chat_voice_status(void)
{
    char ip[24];
    uint16_t port = 0;

    rt_kprintf("语音键: %s\n", voice_key_enabled() ? "on（按住 KEY0 说话）" : "off");
    rt_kprintf("状态: %s%s\n",
               voice_is_busy() ? "处理中" : "空闲",
               voice_is_recording() ? "（录音中）" : "");
    rt_kprintf("音量: %d    MIC 增益: %d%s\n",
               music_get_volume(), voice_get_gain(),
               voice_get_gain() == 0 ? "（自动 AGC）" : " 倍");
    if (ai_get_server(ip, sizeof(ip), &port) == AI_OK)
    {
        rt_kprintf("网关: %s:%u\n", ip, (unsigned)port);
    }
    else
    {
        rt_kprintf("网关: 未设置（先 chat server <PC_IP> [port]）\n");
    }
}

static void chat_voice(int argc, char **argv)
{
    const char *action = (argc > 2) ? argv[2] : "status";

    if ((strcmp(action, "on") == 0) || (strcmp(action, "off") == 0))
    {
        voice_key_enable(strcmp(action, "on") == 0);
        rt_kprintf("KEY0 语音键: %s\n", voice_key_enabled() ? "on" : "off");
        return;
    }

    if (strcmp(action, "vol") == 0)
    {
        if (argc > 3)
        {
            int vol = atoi(argv[3]);

            if ((vol < 0) || (vol > 100))
            {
                rt_kprintf("音量范围 0~100\n");
                return;
            }
            if (music_set_volume(vol) != MUSIC_OK)
            {
                rt_kprintf("设置失败\n");
                return;
            }
        }
        rt_kprintf("音量: %d\n", music_get_volume());
        return;
    }

    if (strcmp(action, "gain") == 0)
    {
        if (argc > 3)
        {
            int gain = atoi(argv[3]);

            if ((gain < 0) || (gain > 15))
            {
                rt_kprintf("MIC 增益范围 0~15（0 = 自动 AGC）\n");
                return;
            }
            voice_set_gain(gain);
        }
        rt_kprintf("MIC 增益: %d%s\n", voice_get_gain(),
                   voice_get_gain() == 0 ? "（自动 AGC）" : " 倍");
        return;
    }

    if ((strcmp(action, "status") == 0) || (strcmp(action, "info") == 0))
    {
        chat_voice_status();
        return;
    }

    rt_kprintf("未知 voice 参数: %s\n", action);
    rt_kprintf("用法: chat voice [on|off|vol <0-100>|gain <0-15>]\n");
}

static void chat_text(int argc, char **argv)
{
    char text[256];
    size_t len = 0;
    int i;
    int ret;

    if (argc < 3)
    {
        rt_kprintf("用法: chat text <text...>\n");
        return;
    }

    /* 把剩下的参数用空格拼成一句话（msh 行缓冲上限 256） */
    text[0] = '\0';
    for (i = 2; i < argc; i++)
    {
        size_t n = strlen(argv[i]);

        if ((len + n + 2u) > sizeof(text))
        {
            rt_kprintf("问题太长（最多 %u 字节）\n", (unsigned)(sizeof(text) - 2u));
            return;
        }
        if (i != 2)
        {
            text[len++] = ' ';
        }
        memcpy(&text[len], argv[i], n);
        len += n;
        text[len] = '\0';
    }

    rt_kprintf("正在请求大模型...（最长等 20 秒）\n");
    ret = ai_ask(text);
    if (ret != AI_OK)
    {
        rt_kprintf("请求失败: %s\n", ai_err_str(ret));
    }
    else
    {
        rt_kprintf("\n");
    }
}

static int chat(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        chat_usage();
        return 0;
    }
    sub = argv[1];

    if (strcmp(sub, "server") == 0)
    {
        uint16_t port = 8765u;
        int ret;

        if (argc < 3)
        {
            rt_kprintf("用法: chat server <ip> [port]\n");
            return 0;
        }
        if (argc > 3)
        {
            int p = atoi(argv[3]);

            if ((p <= 0) || (p > 65535))
            {
                rt_kprintf("端口范围 1~65535\n");
                return 0;
            }
            port = (uint16_t)p;
        }

        ret = ai_set_server(argv[2], port);
        if (ret == AI_OK)
        {
            rt_kprintf("网关: %s:%u\n", argv[2], (unsigned)port);
            rt_kprintf("  确认 PC 上已运行: python tools/llm_bridge.py\n");
        }
        else
        {
            rt_kprintf("设置失败: %s\n", ai_err_str(ret));
        }
        return 0;
    }

    if ((strcmp(sub, "text") == 0) || (strcmp(sub, "ask") == 0))
    {
        chat_text(argc, argv);
        return 0;
    }

    if (strcmp(sub, "voice") == 0)
    {
        chat_voice(argc, argv);
        return 0;
    }

    if (strcmp(sub, "info") == 0)
    {
        chat_voice_status();
        rt_kprintf("模型 / API Key: 由 PC 端 llm_bridge.py 的环境变量决定\n");
        return 0;
    }

    if ((strcmp(sub, "help") == 0) || (strcmp(sub, "?") == 0))
    {
        chat_usage();
        return 0;
    }

    rt_kprintf("未知子命令: %s\n", sub);
    chat_usage();
    return 0;
}
MSH_CMD_EXPORT(chat, chat <text|voice|server|info>: 文本 / 语音对话);

/* 系统初始化（main 之前、调度器已运行）自动启动语音按键线程 */
static int chat_voice_autostart(void)
{
    (void)voice_service_start();
    return 0;
}
INIT_APP_EXPORT(chat_voice_autostart);
