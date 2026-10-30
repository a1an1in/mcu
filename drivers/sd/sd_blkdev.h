/*
 * sd_blkdev.h - SD 卡 → 块设备适配 <drivers/sd/>
 *
 * sd.h 是「卡协议」接口（sd_read_blocks / sd_write_blocks），本文件把它
 * 包成 drivers/block 的通用块设备，让文件系统只认 blkdev 接口。
 *
 * 这一层只有一个函数：注册。注册**不做 I/O**（和 board 初始化的约束一致），
 * 识别卡推迟到 blkdev_init()，也就是第一次 f_mount 的时候。
 *
 * 用法（board 里）：
 *     sd_blkdev_register(&s_sd, "sd0");
 */
#ifndef DRIVERS_SD_SD_BLKDEV_H__
#define DRIVERS_SD_SD_BLKDEV_H__

#include "blkdev.h"
#include "sd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 把 card 注册成名为 name 的块设备（通常是 "sd0"）。
 * card 必须长期有效（board 里的 static 变量）。失败返回 NULL。 */
struct blkdev *sd_blkdev_register(struct sd_card *card, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_SD_SD_BLKDEV_H__ */
