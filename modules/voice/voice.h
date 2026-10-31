/*
 * voice.h - 按住 KEY0 说话的流式语音对话 <modules/voice/>
 *
 * 流程（半双工、push-to-talk）：
 *   按住 KEY0 → VS1053 录音，同时 512B/块 TCP 发给 PC 的 /voice
 *   松开 KEY0 → 半关闭发送，PC 做 ASR → LLM → TTS
 *   收到 audio/wav → 边下边喂 VS1053 播放
 *
 * 不落 SD 卡，录音数据直接走网络；PC 端见 tools/llm_bridge.py。
 */
#ifndef MODULES_VOICE_VOICE_H__
#define MODULES_VOICE_VOICE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 启动按键监听线程（幂等）。由 msh 命令文件里的 INIT_APP_EXPORT 调用。 */
int voice_service_start(void);

/* 打开/关闭 KEY0 语音键（默认打开）。 */
void voice_key_enable(int on);
int voice_key_enabled(void);

int voice_is_busy(void);        /* 正在录音/上传/等待回答 */
int voice_is_recording(void);   /* 正在采集 MIC */

#ifdef __cplusplus
}
#endif

#endif /* MODULES_VOICE_VOICE_H__ */
