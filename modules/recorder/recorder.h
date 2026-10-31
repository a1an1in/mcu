/*
 * recorder.h - WAV 录音模块（VS1053 + FatFs）<modules/recorder/>
 *
 * 录音格式固定为 **PCM 16 位单声道 8kHz**（与第 55 章一致）：
 *   VS1053 MIC 输入 → 内部 PCM FIFO → 读 HDAT0 → 写 SD 卡 WAV
 *
 * 文件自动命名到 0:/RECORDER/REC00000.WAV，目录不存在会自动创建。
 * 依赖 fs 命令挂载的卷，所以先 `fs mount`。
 */
#ifndef MODULES_RECORDER_RECORDER_H__
#define MODULES_RECORDER_RECORDER_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORDER_OK             0
#define RECORDER_ERR_PARAM      (-1)
#define RECORDER_ERR_BUSY       (-2)
#define RECORDER_ERR_NODEV      (-3)
#define RECORDER_ERR_IO         (-4)

#define RECORDER_SAMPLE_RATE    8000u   /* 固定 8kHz / 16bit / 单声道 */
#define RECORDER_DEFAULT_GAIN   4u      /* 0 = 自动 AGC；1~15 = 1024*gain */

/* 开始录音。max_seconds = 0 表示不按时间自动停（用 recorder_stop 停）。
 * gain: 0 = 自动 AGC，1~15 → 1~15 倍（1024 为 1 倍）。 */
int recorder_start(uint32_t max_seconds, uint16_t gain);

/* 停止并等线程收尾（回填 WAV 头、关文件），最多等 2 秒。 */
int recorder_stop(void);

int recorder_is_recording(void);

/* 当前/最后一个录音文件名（如 "0:/RECORDER/REC00001.WAV"）；没录过返回 NULL。 */
const char *recorder_current_file(void);

/* 已录字节数 / 时长（秒，按 8000Hz×16bit×单声道折算）。 */
uint32_t recorder_bytes(void);
uint32_t recorder_seconds(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_RECORDER_RECORDER_H__ */
