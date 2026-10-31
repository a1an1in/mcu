/*
 * wav.h - WAV(PCM) 文件头工具 <modules/recorder/>
 *
 * 只做 44 字节标准 WAV 头的生成与 data 长度回填，不碰文件系统/硬件，
 * 方便在宿主机上单测。
 */
#ifndef MODULES_RECORDER_WAV_H__
#define MODULES_RECORDER_WAV_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAV_HEADER_SIZE     44u

/* 生成标准 PCM WAV 头（data 长度先填 0）。 */
void wav_header_init(uint8_t hdr[WAV_HEADER_SIZE], uint32_t sample_rate,
                     uint16_t channels, uint16_t bits_per_sample);

/* 录音结束后回填 RIFF 长度（36 + data）和 data 长度。 */
void wav_header_set_data_size(uint8_t hdr[WAV_HEADER_SIZE], uint32_t data_bytes);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_RECORDER_WAV_H__ */
