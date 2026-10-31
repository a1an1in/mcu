/*
 * chat_cmd.c - 大模型对话的 msh 命令 <modules/msh/>
 *
 *   chat server <ip> [port]   设置 PC 网关（默认端口 8765）
 *   chat ask <text...>        发一个问题并打印回答
 *   chat info                 查看当前网关配置
 *
 * 板子只发明文 HTTP 到 PC；HTTPS 与 API Key 都在 PC 的
 * tools/llm_bridge.py 里处理。
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"

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

static void ai_usage(void)
{
    rt_kprintf("用法: chat <server|ask|info>\n");
    rt_kprintf("  chat server <ip> [port]   设置 PC 网关（默认 8765）\n");
    rt_kprintf("  chat ask <text...>        问大模型，例: chat ask 你好\n");
    rt_kprintf("  chat info                 查看网关配置\n");
    rt_kprintf("  说明: 板子发明文 HTTP 到 PC，HTTPS/API Key 在 PC 端处理\n");
}

static int chat(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        ai_usage();
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

    if (strcmp(sub, "ask") == 0)
    {
        char text[256];
        size_t len = 0;
        int i;
        int ret;

        if (argc < 3)
        {
            rt_kprintf("用法: chat ask <text...>\n");
            return 0;
        }

        /* 把剩下的参数用空格拼成一句话（msh 行缓冲上限 256） */
        text[0] = '\0';
        for (i = 2; i < argc; i++)
        {
            size_t n = strlen(argv[i]);

            if ((len + n + 2u) > sizeof(text))
            {
                rt_kprintf("问题太长（最多 %u 字节）\n", (unsigned)(sizeof(text) - 2u));
                return 0;
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
        return 0;
    }

    if (strcmp(sub, "info") == 0)
    {
        char ip[24];
        uint16_t port = 0;

        if (ai_get_server(ip, sizeof(ip), &port) != AI_OK)
        {
            rt_kprintf("网关: 未设置（chat server <PC_IP> [port]）\n");
            return 0;
        }
        rt_kprintf("网关: %s:%u\n", ip, (unsigned)port);
        rt_kprintf("模型 / API Key: 由 PC 端 llm_bridge.py 的环境变量决定\n");
        return 0;
    }

    if ((strcmp(sub, "help") == 0) || (strcmp(sub, "?") == 0))
    {
        ai_usage();
        return 0;
    }

    rt_kprintf("未知子命令: %s\n", sub);
    ai_usage();
    return 0;
}
MSH_CMD_EXPORT(chat, chat <server|ask|info>: 通过 PC 网关和大模型对话);
