/*
 * recorder.c - WAV 录音模块（VS1053 → FatFs）<modules/recorder/>
 *
 * 线程模型与 modules/music 对称：
 *   recorder_start() 建线程后立即返回；
 *   线程循环「等 FIFO 够 512 字节 → vs1053_record_read → f_write」；
 *   recorder_stop() 置停止标志并等线程回填 WAV 头、关闭文件。
 *
 * 为什么每次读 512 字节：VS1053 录音 FIFO 约 1024 个 16 位字，
 * HDAT1 在 256~896 之间时读 256 个字（512B）最稳，约 32ms 一块。
 */
#include <string.h>

#include <osal.h>

#include "ff.h"
#include "recorder.h"
#include "vs1053.h"
#include "wav.h"

#define RECORDER_DIR            "0:/RECORDER"
#define RECORDER_BUF_SIZE       512u
#define RECORDER_STACK_SIZE     2048u
#define RECORDER_PRIORITY       16u
#define RECORDER_STOP_TIMEOUT   200u    /* 200 × 10ms = 2s */

static osal_thread_t s_thread;
static volatile uint8_t s_recording;
static volatile uint8_t s_stop;
static volatile uint32_t s_bytes;
static uint32_t s_max_seconds;
static uint16_t s_gain_reg;
static char s_name[32];
static uint8_t s_buf[RECORDER_BUF_SIZE] __attribute__((aligned(4)));

/* ============================ 文件名 ============================ */

static void recorder_make_name(char *out, unsigned int idx)
{
    static const char prefix[] = "0:/RECORDER/REC";
    const size_t n = sizeof(prefix) - 1u;
    int i;

    memcpy(out, prefix, n);
    for (i = 4; i >= 0; i--)
    {
        out[n + (size_t)i] = (char)('0' + (idx % 10u));
        idx /= 10u;
    }
    memcpy(&out[n + 5u], ".WAV", 5);
}

/* 找一个不存在的 RECxxxxx.WAV，返回 0 成功 */
static int recorder_find_name(char *out)
{
    FILINFO fno;
    unsigned int idx;

    for (idx = 0; idx < 65535u; idx++)
    {
        recorder_make_name(out, idx);
        if (f_stat(out, &fno) == FR_NO_FILE)
        {
            return 0;
        }
    }
    return -1;
}

/* ============================ 录音线程 ============================ */

static void recorder_thread(void *param)
{
    struct vs1053 *dev = vs1053_get_default();
    FIL fil;
    FRESULT fr;
    UINT bw;
    uint8_t hdr[WAV_HEADER_SIZE];
    uint32_t limit;

    (void)param;

    if (dev == NULL)
    {
        osal_printf("[record] 没有 VS1053 设备\n");
        goto out;
    }

    /* 目录不存在就建：FR_EXIST 不算错 */
    fr = f_mkdir(RECORDER_DIR);
    if ((fr != FR_OK) && (fr != FR_EXIST))
    {
        osal_printf("[record] 创建 %s 失败 (FRESULT=%d)\n", RECORDER_DIR, (int)fr);
        goto out;
    }

    if (recorder_find_name(s_name) != 0)
    {
        osal_printf("[record] %s 下没有可用文件名\n", RECORDER_DIR);
        goto out;
    }

    fr = f_open(&fil, s_name, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        if (fr == FR_NOT_ENABLED)
        {
            osal_printf("[record] 卷还没挂载，先 fs mount\n");
        }
        else
        {
            osal_printf("[record] 创建 %s 失败 (FRESULT=%d)\n", s_name, (int)fr);
        }
        goto out;
    }

    /* 先写 44 字节头（data 长度 = 0），结束时回填 */
    wav_header_init(hdr, RECORDER_SAMPLE_RATE, 1u, 16u);
    if ((f_write(&fil, hdr, WAV_HEADER_SIZE, &bw) != FR_OK) ||
        (bw != WAV_HEADER_SIZE))
    {
        osal_printf("[record] 写 WAV 头失败\n");
        (void)f_close(&fil);
        (void)f_unlink(s_name);
        goto out;
    }

    if (vs1053_record_start(dev, RECORDER_SAMPLE_RATE, s_gain_reg) != VS1053_OK)
    {
        osal_printf("[record] VS1053 进入录音模式失败\n");
        (void)f_close(&fil);
        (void)f_unlink(s_name);
        goto out;
    }

    s_bytes = 0;
    s_recording = 1u;
    limit = (s_max_seconds == 0u) ? 0u : (s_max_seconds * RECORDER_SAMPLE_RATE * 2u);

    while (s_stop == 0u)
    {
        if ((limit != 0u) && (s_bytes >= limit))
        {
            break;
        }

        if (vs1053_record_read(dev, s_buf, RECORDER_BUF_SIZE) != VS1053_OK)
        {
            osal_printf("[record] 读录音 FIFO 超时/失败，结束\n");
            break;
        }

        fr = f_write(&fil, s_buf, RECORDER_BUF_SIZE, &bw);
        if ((fr != FR_OK) || (bw != RECORDER_BUF_SIZE))
        {
            osal_printf("[record] 写文件失败 (FRESULT=%d, %u/%u)\n",
                        (int)fr, (unsigned)bw, (unsigned)RECORDER_BUF_SIZE);
            break;
        }
        s_bytes += bw;
    }

    (void)vs1053_record_stop(dev);

    /* 回填头：RIFF 长度和 data 长度 */
    wav_header_set_data_size(hdr, s_bytes);
    (void)f_lseek(&fil, 0);
    (void)f_write(&fil, hdr, WAV_HEADER_SIZE, &bw);
    (void)f_close(&fil);

    osal_printf("\n[record] 已保存 %s（%u 字节，约 %u 秒）",
                s_name, (unsigned)s_bytes, (unsigned)(s_bytes / 16000u));

out:
    s_recording = 0u;
    s_thread = OSAL_NULL;
}

/* ============================ 对外接口 ============================ */

int recorder_start(uint32_t max_seconds, uint16_t gain)
{
    if (gain > 15u)
    {
        return RECORDER_ERR_PARAM;
    }
    if (vs1053_get_default() == NULL)
    {
        return RECORDER_ERR_NODEV;
    }
    if (s_recording != 0u)
    {
        return RECORDER_ERR_BUSY;
    }

    s_stop = 0u;
    s_bytes = 0u;
    s_max_seconds = max_seconds;
    s_gain_reg = (gain == 0u) ? 0u : (uint16_t)(gain * 1024u);
    s_name[0] = '\0';
    s_recording = 1u;                   /* 先占位，避免重复启动 */

    if (osal_thread_create(&s_thread, "record", recorder_thread, NULL,
                           NULL, RECORDER_STACK_SIZE, RECORDER_PRIORITY) != OSAL_OK)
    {
        s_recording = 0u;
        return RECORDER_ERR_IO;
    }
    return RECORDER_OK;
}

int recorder_stop(void)
{
    uint32_t i;

    if (s_recording == 0u)
    {
        return RECORDER_OK;
    }

    s_stop = 1u;

    for (i = 0; i < RECORDER_STOP_TIMEOUT; i++)
    {
        if (s_recording == 0u)
        {
            return RECORDER_OK;
        }
        osal_thread_sleep_ms(10);
    }
    return RECORDER_ERR_BUSY;
}

int recorder_is_recording(void)
{
    return (s_recording != 0u) ? 1 : 0;
}

const char *recorder_current_file(void)
{
    return (s_name[0] != '\0') ? s_name : NULL;
}

uint32_t recorder_bytes(void)
{
    return s_bytes;
}

uint32_t recorder_seconds(void)
{
    return s_bytes / 16000u;            /* 8000Hz × 16bit × 单声道 */
}
