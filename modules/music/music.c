/*
 * music.c - 音乐播放模块（FatFs → VS1053）<modules/music/>
 *
 * 只做「读文件 → 喂解码器」这一件事：
 *   f_open/f_read 取 4KB
 *     └ vs1053_write_data() 内部按 32B 切块、等 DREQ
 * 播放线程与 msh 之间用两个 volatile 标志（s_stop / s_playing）通信，
 * 不共享 FatFs 对象，也不自己建卷 —— 挂载是 modules/msh/fs 命令做的。
 */
#include <string.h>

#include <osal.h>

#include "ff.h"
#include "fs_util.h"
#include "music.h"
#include "vs1053.h"

#define MUSIC_BUF_SIZE      4096u
#define MUSIC_STACK_SIZE    4096u
#define MUSIC_PRIORITY      15u
#define MUSIC_STOP_TIMEOUT  200u    /* 200 × 10ms = 2s */

static osal_thread_t s_thread;
static volatile uint8_t s_playing;
static volatile uint8_t s_stop;
static volatile uint8_t s_volume = 80u;
static char s_path[FS_PATH_MAX];
static uint8_t s_buf[MUSIC_BUF_SIZE] __attribute__((aligned(4)));

/* ============================ 播放线程 ============================ */

static void music_thread(void *param)
{
    struct vs1053 *dev = vs1053_get_default();
    FIL fil;
    FRESULT fr;
    UINT br;
    uint32_t i;

    (void)param;

    if (dev == NULL)
    {
        osal_printf("[music] 没有 VS1053 设备（board 里 bind 失败？）\n");
        goto out;
    }

    if (vs1053_init(dev) != VS1053_OK)
    {
        osal_printf("[music] VS1053 初始化失败（检查 VS1053 是否焊好 / RST 是否被拉低）\n");
        goto out;
    }

    (void)vs1053_set_volume(dev, (int)s_volume);
    (void)vs1053_set_speaker(dev, 1);

    fr = f_open(&fil, s_path, FA_READ);
    if (fr != FR_OK)
    {
        if (fr == FR_NOT_ENABLED)
        {
            osal_printf("[music] 卷还没挂载，先 fs mount\n");
        }
        else
        {
            osal_printf("[music] 打开 %s 失败 (FRESULT=%d)\n", s_path, (int)fr);
        }
        goto out;
    }

    /* 切歌/重播：先清掉上一首的残留，再清零解码时间 */
    (void)vs1053_cancel(dev);
    vs1053_reset_decode_time(dev);

    while (s_stop == 0u)
    {
        fr = f_read(&fil, s_buf, sizeof(s_buf), &br);
        if (fr != FR_OK)
        {
            osal_printf("[music] 读文件失败 (FRESULT=%d)\n", (int)fr);
            break;
        }
        if (br == 0u)
        {
            break;                      /* EOF */
        }

        for (i = 0; (i < br) && (s_stop == 0u); i += 32u)
        {
            uint16_t n = (uint16_t)((br - i > 32u) ? 32u : (br - i));

            if (vs1053_write_data(dev, &s_buf[i], n) != VS1053_OK)
            {
                osal_printf("[music] 送数据超时/失败，停止播放\n");
                s_stop = 1u;
                break;
            }
        }
    }

    (void)f_close(&fil);

    /* 正常放完或被停止：清掉解码器里的残留，避免下一首开头爆音 */
    (void)vs1053_cancel(dev);

out:
    s_playing = 0u;
    s_thread = OSAL_NULL;
}

/* ============================ 对外接口 ============================ */

int music_play(const char *path)
{
    if (path == NULL)
    {
        return MUSIC_ERR_PARAM;
    }
    if (vs1053_get_default() == NULL)
    {
        return MUSIC_ERR_NODEV;
    }
    if (s_playing != 0u)
    {
        return MUSIC_ERR_BUSY;
    }
    if (fs_build_path(path, s_path, sizeof(s_path)) != 0)
    {
        return MUSIC_ERR_PARAM;
    }

    s_stop = 0u;
    s_playing = 1u;

    if (osal_thread_create(&s_thread, "music", music_thread, NULL,
                           NULL, MUSIC_STACK_SIZE, MUSIC_PRIORITY) != OSAL_OK)
    {
        s_playing = 0u;
        return MUSIC_ERR_IO;
    }
    return MUSIC_OK;
}

int music_stop(void)
{
    uint32_t i;

    if (s_playing == 0u)
    {
        return MUSIC_OK;
    }

    s_stop = 1u;

    for (i = 0; i < MUSIC_STOP_TIMEOUT; i++)
    {
        if (s_playing == 0u)
        {
            return MUSIC_OK;
        }
        osal_thread_sleep_ms(10);
    }
    return MUSIC_ERR_BUSY;
}

int music_set_volume(int vol)
{
    struct vs1053 *dev = vs1053_get_default();

    if ((vol < 0) || (vol > 100))
    {
        return MUSIC_ERR_PARAM;
    }

    s_volume = (uint8_t)vol;

    if ((dev != NULL) && (dev->ready != 0u))
    {
        return (vs1053_set_volume(dev, vol) == VS1053_OK) ? MUSIC_OK : MUSIC_ERR_IO;
    }
    return MUSIC_OK;
}

int music_get_volume(void)
{
    return (int)s_volume;
}

int music_is_playing(void)
{
    return (s_playing != 0u) ? 1 : 0;
}

const char *music_current_file(void)
{
    return (s_playing != 0u) ? s_path : NULL;
}

uint16_t music_decode_time(void)
{
    struct vs1053 *dev = vs1053_get_default();

    if ((s_playing == 0u) || (dev == NULL) || (dev->ready == 0u))
    {
        return 0;
    }
    return vs1053_decode_time(dev);
}

uint16_t music_bitrate(void)
{
    struct vs1053 *dev = vs1053_get_default();

    if ((s_playing == 0u) || (dev == NULL) || (dev->ready == 0u))
    {
        return 0;
    }
    return vs1053_bitrate(dev);
}

int music_test(void)
{
    struct vs1053 *dev = vs1053_get_default();

    if (s_playing != 0u)
    {
        return MUSIC_ERR_BUSY;
    }
    if (dev == NULL)
    {
        return MUSIC_ERR_NODEV;
    }
    return (vs1053_sine_test(dev) == VS1053_OK) ? MUSIC_OK : MUSIC_ERR_IO;
}
