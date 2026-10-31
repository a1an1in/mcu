/*
 * ai.h - 通过局域网网关和大模型对话 <modules/ai/>
 *
 * 板子本身不做 TLS：它把问题用**明文 HTTP POST**发给 PC 上的
 * tools/llm_bridge.py，由 PC 转发到 https://api.deepseek.com（或别的兼容
 * OpenAI /chat/completions 的服务）。API Key 只存在 PC 上，不烧进固件。
 *
 * 这样做的原因：DeepSeek 只提供 HTTPS，而 STM32F103 上跑 TLS（mbedTLS）
 * 至少要几十 KB RAM 和上百 KB Flash，还要处理证书/时间，性价比很低。
 *
 * 用法（msh）：
 *   ai server 192.168.1.100 8765
 *   ai ask 用一个 GPIO 控制 LED 的代码怎么写
 */
#ifndef MODULES_AI_AI_H__
#define MODULES_AI_AI_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_OK               0
#define AI_ERR_PARAM        (-1)
#define AI_ERR_NOSERVER     (-2)    /* 还没 ai server */
#define AI_ERR_NET          (-3)    /* 连不上 / 发送失败 */
#define AI_ERR_TOOBIG       (-4)    /* 问题太长，请求缓冲区装不下 */

/* 设置 PC 网关地址（点分十进制 + 端口）。成功返回 0。 */
int ai_set_server(const char *ip, uint16_t port);

/* 取当前网关配置（给 msh 打印用）。未配置返回 -1。 */
int ai_get_server(char *ip_buf, unsigned int ip_size, uint16_t *port);

/* 发一个问题并打印回答（UTF-8 分块输出）。成功返回 0。 */
int ai_ask(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_AI_AI_H__ */
