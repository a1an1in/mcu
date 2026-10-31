/*
 * record_cmd.c - WAV 录音的 msh 命令 <modules/msh/>
 *
 *   record start [seconds] [gain]   开始录音（0 或不写 = 手动 stop 才停）
 *   record stop                     停止并保存
 *   record info                     状态 / 文件 / 时长
 *
 * gain：0 = 自动 AGC；1~15 = 1~15 倍（默认 4）。
 * 录音固定 PCM 16bit / 单声道 / 8kHz，文件在 0:/RECORDER/RECxxxxx.WAV。
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"
#include "recorder.h"

static const char *record_err_str(int ret)
{
    switch (ret)
    {
    case RECORDER_OK:           return "OK";
    case RECORDER_ERR_PARAM:    return "参数非法（秒数/增益越界）";
    case RECORDER_ERR_BUSY:     return "正在录音，先 record stop";
    case RECORDER_ERR_NODEV:    return "没有 VS1053";
    case RECORDER_ERR_IO:       return "IO 错误（建线程 / 文件 / 解码器）";
    default:                    return "未知错误";
    }
}

static void record_usage(void)
{
    rt_kprintf("用法: record <start|stop|info>\n");
    rt_kprintf("  record start [seconds] [gain]  开始录音（0/省略=手动停）\n");
    rt_kprintf("  record stop                    停止并保存\n");
    rt_kprintf("  record info                    查看录音状态\n");
    rt_kprintf("  gain: 0=自动AGC，1~15=倍数（默认 4）\n");
}

static int record(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        record_usage();
        return 0;
    }
    sub = argv[1];

    if (strcmp(sub, "start") == 0)
    {
        uint32_t seconds = 0;
        uint16_t gain = RECORDER_DEFAULT_GAIN;
        int ret;

        if (argc > 2)
        {
            seconds = (uint32_t)atoi(argv[2]);
            if (seconds > 3600u)
            {
                rt_kprintf("秒数最大 3600（0 = 一直录到 record stop）\n");
                return 0;
            }
        }
        if (argc > 3)
        {
            int g = atoi(argv[3]);

            if ((g < 0) || (g > 15))
            {
                rt_kprintf("增益范围 0~15（0 = 自动 AGC）\n");
                return 0;
            }
            gain = (uint16_t)g;
        }

        /* 录音和解码共用 VS1053：放歌时不允许开始录音 */
        if (music_is_playing() != 0)
        {
            rt_kprintf("正在播放音乐，先 mplayer stop 再录音\n");
            return 0;
        }

        ret = recorder_start(seconds, gain);
        if (ret == RECORDER_OK)
        {
            rt_kprintf("开始录音（%s，增益 %u%s）\n",
                       (seconds == 0u) ? "手动停止" : "定时",
                       (unsigned)gain, (gain == 0u) ? "（自动 AGC）" : " 倍");
            rt_kprintf("  record stop 停止，record info 查看\n");
        }
        else
        {
            rt_kprintf("录音失败: %s\n", record_err_str(ret));
        }
        return 0;
    }

    if (strcmp(sub, "stop") == 0)
    {
        int ret = recorder_stop();

        if (ret == RECORDER_OK)
        {
            rt_kprintf("已停止，文件: %s\n",
                       recorder_current_file() != NULL ? recorder_current_file() : "(无)");
        }
        else
        {
            rt_kprintf("停止失败: %s\n", record_err_str(ret));
        }
        return 0;
    }

    if (strcmp(sub, "info") == 0)
    {
        if (recorder_is_recording() == 0)
        {
            if (recorder_current_file() != NULL)
            {
                rt_kprintf("状态: 空闲（最后一次: %s，%u 字节，约 %u 秒）\n",
                           recorder_current_file(),
                           (unsigned)recorder_bytes(),
                           (unsigned)recorder_seconds());
            }
            else
            {
                rt_kprintf("状态: 空闲（还没录过）\n");
            }
        }
        else
        {
            rt_kprintf("状态: 录音中\n");
            rt_kprintf("文件: %s\n",
                       recorder_current_file() != NULL ? recorder_current_file() : "(准备中)");
            rt_kprintf("时长: %u 秒\n", (unsigned)recorder_seconds());
            rt_kprintf("大小: %u 字节\n", (unsigned)recorder_bytes());
        }
        return 0;
    }

    if ((strcmp(sub, "help") == 0) || (strcmp(sub, "?") == 0))
    {
        record_usage();
        return 0;
    }

    rt_kprintf("未知子命令: %s\n", sub);
    record_usage();
    return 0;
}
MSH_CMD_EXPORT(record, record <start|stop|info>: VS1053 WAV 录音);
