/*
 * chat.h - 文本 / 语音对话的统一接口 <modules/chat/>
 *
 * 板子只发明文 HTTP 到 PC 上的 tools/llm_bridge.py：
 *   /ask    文本 JSON → 文本回答
 *   /voice  流式 8k/16bit/单声道 PCM → ASR → LLM → TTS → 音频
 * HTTPS 与 API Key 都在 PC 端，不烧进固件。
 *
 * 用法（msh）：
 *   chat server 192.168.1.100 8765
 *   chat text 你好
 *   按住 KEY0 说话（语音键默认打开）
 */
#ifndef MODULES_CHAT_CHAT_H__
#define MODULES_CHAT_CHAT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回值 */
#define CHAT_OK             0
#define CHAT_ERR_PARAM      (-1)
#define CHAT_ERR_NOSERVER   (-2)    /* 还没 chat server */
#define CHAT_ERR_NET        (-3)    /* 连不上 / 发送失败 */
#define CHAT_ERR_TOOBIG     (-4)    /* 问题太长，请求缓冲区装不下 */

/* ============================ 网关配置 ============================ */

/* 设置 PC 网关（点分十进制 + 端口），文本和语音共用。 */
int chat_server_set(const char *ip, uint16_t port);

/* 取当前网关配置（给 msh 打印用）。未配置返回 -1。 */
int chat_server_get(char *ip_buf, unsigned int ip_size, uint16_t *port);

/* ============================ 文本对话 ============================ */

/* 发一个问题并打印回答（UTF-8 分块输出）。 */
int chat_text_ask(const char *text);

/* ============================ 语音对话 ============================ */

/* 启动 KEY0 监听线程（幂等；由 INIT_APP_EXPORT 自动调用）。 */
int chat_voice_service_start(void);

/* 打开/关闭 KEY0 语音键（默认打开）。 */
void chat_voice_key_enable(int on);
int chat_voice_key_enabled(void);

int chat_voice_is_busy(void);       /* 正在录音/上传/等待回答 */
int chat_voice_is_recording(void);  /* 正在采集 MIC */

/* MIC 增益：0 = 自动 AGC；1~15 = 1024×gain（默认 4）。 */
void chat_voice_set_gain(int gain);
int chat_voice_get_gain(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_CHAT_CHAT_H__ */
