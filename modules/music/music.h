/*
 * music.h - 音乐播放模块（FatFs + VS1053）<modules/music/>
 *
 * 依赖：modules/fs（FatFs 卷）+ drivers/audio（VS1053）。
 * 播放放在独立线程里，msh 命令立即返回；`music stop` 会等线程真正退出。
 *
 * ⚠️ 播放文件用的就是 fs 命令挂载的那个卷（FatFs 的卷是全局的），
 *    所以先 `fs mount`，再 `music play /MUSIC/xxx.mp3`。
 *    播放期间不要 `fs umount` / `fs mkfs`。
 */
#ifndef MODULES_MUSIC_MUSIC_H__
#define MODULES_MUSIC_MUSIC_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_OK            0
#define MUSIC_ERR_PARAM     (-1)    /* 路径/音量非法 */
#define MUSIC_ERR_BUSY      (-2)    /* 正在播放 / 停不下来 */
#define MUSIC_ERR_NODEV     (-3)    /* 没有 VS1053 */
#define MUSIC_ERR_IO        (-4)    /* 打不开文件 / 解码器错误 */

/* 播放一个文件（mp3/wav/wma/mid；flac/ogg 需要额外 patch，不保证）。 */
int music_play(const char *path);

/* 停止播放并等线程退出。返回 MUSIC_OK / MUSIC_ERR_BUSY。 */
int music_stop(void);

/* 音量 0~100。播放中也可以调用。 */
int music_set_volume(int vol);
int music_get_volume(void);

int music_is_playing(void);
const char *music_current_file(void);

/* 播放状态（秒 / kbps）；没播放时返回 0。 */
uint16_t music_decode_time(void);
uint16_t music_bitrate(void);

/* 正弦测试（会打断当前播放，调用方应先 stop）。 */
int music_test(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_MUSIC_MUSIC_H__ */
