/*
 * fs_util.h - 文件系统的小工具（与 OS 无关）<modules/fs/>
 *
 * 目前只有递归删除：FatFs 的 f_unlink() 只能删文件或空目录，
 * 想删一整棵目录树得自己遍历。放在这里而不是 msh 命令里，
 * 是为了能在 host 单测（RAM 盘）上直接验证。
 */
#ifndef MODULES_FS_FS_UTIL_H__
#define MODULES_FS_FS_UTIL_H__

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 递归深度上限。msh 线程栈只有 4KB，一层递归约 120 字节，
 * 8 层留足余量；正常目录不会这么深（再深会返回 FR_INVALID_PARAMETER）。 */
#define FS_REMOVE_MAX_DEPTH     8

/* 递归删除 path（文件直接删；目录则先清空再删自己）。
 *
 * path 必须带盘符，如 "0:/music"（不能是根目录 "0:"）。
 * buf/buf_size 是调用方给的路径拼接工作区，建议 >= 128 字节；
 * 函数内部会在 buf 里拼子路径，调用返回后内容无意义。
 *
 * 返回 FR_OK / FR_NO_FILE / FR_DENIED（只读）/ FR_INVALID_NAME（根或太长）/
 * FR_INVALID_PARAMETER（参数错或目录太深）/ 底层错误。
 */
FRESULT fs_remove_tree(const TCHAR *path, TCHAR *buf, UINT buf_size);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_FS_FS_UTIL_H__ */
