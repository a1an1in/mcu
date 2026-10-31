/*
 * music_cmd.c - 音乐播放器的 msh 命令 <modules/msh/>
 *
 * 只调 modules/music（再往下是 FatFs + VS1053），不碰 SPI/GPIO 寄存器。
 *
 *   music play <path>   播放一个文件（先 fs mount）
 *   music stop          停止
 *   music vol [0-100]   查询/设置音量
 *   music info          播放状态 / 解码时间 / 码率
 *   music test          正弦测试（喇叭“嘀——”）
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "music.h"
#include "recorder.h"

static const char *music_err_str(int ret)
{
    switch (ret)
    {
    case MUSIC_OK:          return "OK";
    case MUSIC_ERR_PARAM:   return "参数非法（路径为空 / 音量不在 0~100）";
    case MUSIC_ERR_BUSY:    return "正在播放，先 music stop";
    case MUSIC_ERR_NODEV:   return "没有 VS1053（板级 bind 失败）";
    case MUSIC_ERR_IO:      return "IO 错误（文件打不开 / 解码器无响应）";
    default:                return "未知错误";
    }
}

static void music_usage(void)
{
    rt_kprintf("用法: music <子命令> [参数]\n");
    rt_kprintf("  music play <path>   播放文件，例: music play /MUSIC/a.mp3\n");
    rt_kprintf("  music stop          停止播放\n");
    rt_kprintf("  music vol [0-100]   查询/设置音量\n");
    rt_kprintf("  music info          播放状态 / 时间 / 码率\n");
    rt_kprintf("  music test          正弦测试\n");
}

static void music_do_info(void)
{
    uint16_t t;
    uint16_t br;

    if (music_is_playing() == 0)
    {
        rt_kprintf("状态: 空闲\n");
        rt_kprintf("音量: %d\n", music_get_volume());
        return;
    }

    t = music_decode_time();
    br = music_bitrate();

    rt_kprintf("状态: 播放中\n");
    rt_kprintf("文件: %s\n", music_current_file());
    rt_kprintf("时间: %u:%02u\n", (unsigned)(t / 60u), (unsigned)(t % 60u));
    rt_kprintf("码率: %u kbps\n", (unsigned)br);
    rt_kprintf("音量: %d\n", music_get_volume());
}

static int music(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        music_usage();
        return 0;
    }
    sub = argv[1];

    if (strcmp(sub, "play") == 0)
    {
        int ret;

        if (recorder_is_recording() != 0)
        {
            rt_kprintf("正在录音，先 record stop 再播放\n");
            return 0;
        }
        if (argc < 3)
        {
            rt_kprintf("用法: music play <path>\n");
            return 0;
        }
        ret = music_play(argv[2]);
        if (ret == MUSIC_OK)
        {
            rt_kprintf("开始播放 %s（%s）\n", argv[2], "music stop 可停止");
        }
        else
        {
            rt_kprintf("播放失败: %s\n", music_err_str(ret));
        }
        return 0;
    }

    if (strcmp(sub, "stop") == 0)
    {
        int ret = music_stop();

        if (ret == MUSIC_OK)
        {
            rt_kprintf("已停止\n");
        }
        else
        {
            rt_kprintf("停止失败: %s\n", music_err_str(ret));
        }
        return 0;
    }

    if (strcmp(sub, "vol") == 0)
    {
        if (argc < 3)
        {
            rt_kprintf("音量: %d\n", music_get_volume());
            return 0;
        }
        else
        {
            int vol = atoi(argv[2]);
            int ret = music_set_volume(vol);

            if (ret == MUSIC_OK)
            {
                rt_kprintf("音量: %d\n", music_get_volume());
            }
            else
            {
                rt_kprintf("设置失败: %s\n", music_err_str(ret));
            }
            return 0;
        }
    }

    if (strcmp(sub, "info") == 0)
    {
        music_do_info();
        return 0;
    }

    if (strcmp(sub, "test") == 0)
    {
        int ret;

        if (recorder_is_recording() != 0)
        {
            rt_kprintf("正在录音，先 record stop 再做正弦测试\n");
            return 0;
        }
        ret = music_test();

        if (ret == MUSIC_OK)
        {
            rt_kprintf("正弦测试完成\n");
        }
        else
        {
            rt_kprintf("测试失败: %s\n", music_err_str(ret));
        }
        return 0;
    }

    if ((strcmp(sub, "help") == 0) || (strcmp(sub, "?") == 0))
    {
        music_usage();
        return 0;
    }

    rt_kprintf("未知子命令: %s\n", sub);
    music_usage();
    return 0;
}
MSH_CMD_EXPORT(music, music <play|stop|vol|info|test>: VS1053 音乐播放器);
