/*
 * fs_util.c - 文件系统的小工具（与 OS 无关）<modules/fs/>
 *
 * 递归删除的实现要点：
 *   1) 只用一个路径缓冲（调用方传入），靠「追加子名 / 回溯置 '\0'」复用，
 *      避免每层递归再开一个 128B 的栈数组（msh 线程栈只有 4KB）。
 *   2) 先 f_opendir 把目录项读出来，再改缓冲拼子路径；DIR 里存的是簇号，
 *      不引用路径字符串，所以改缓冲是安全的。
 *   3) 深度超过 FS_REMOVE_MAX_DEPTH 直接报错，不做「删一半再失败」。
 */
#include <string.h>

#include "fs_util.h"

static FRESULT rm_tree(TCHAR *buf, size_t len, size_t buf_size, int depth)
{
    DIR dir;
    FILINFO fno;
    FRESULT fr;

    if (depth >= FS_REMOVE_MAX_DEPTH)
    {
        return FR_INVALID_PARAMETER;
    }

    fr = f_opendir(&dir, buf);
    if (fr != FR_OK)
    {
        return fr;
    }

    for (;;)
    {
        size_t n;

        fr = f_readdir(&dir, &fno);
        if ((fr != FR_OK) || (fno.fname[0] == '\0'))
        {
            break;
        }
        if ((strcmp(fno.fname, ".") == 0) || (strcmp(fno.fname, "..") == 0))
        {
            continue;
        }

        n = strlen(fno.fname);
        if ((len + 1u + n) >= buf_size)
        {
            fr = FR_INVALID_NAME;
            break;
        }

        /* buf = <当前目录>/<子项> */
        buf[len] = '/';
        memcpy(&buf[len + 1u], fno.fname, n + 1u);

        if ((fno.fattrib & AM_DIR) != 0u)
        {
            fr = rm_tree(buf, len + 1u + n, buf_size, depth + 1);
        }
        else
        {
            fr = f_unlink(buf);
        }

        buf[len] = '\0';            /* 回溯：恢复成当前目录 */
        if (fr != FR_OK)
        {
            break;
        }
    }

    (void)f_closedir(&dir);

    if (fr != FR_OK)
    {
        return fr;
    }

    /* 目录已空，删掉它自己 */
    return f_unlink(buf);
}

FRESULT fs_remove_tree(const TCHAR *path, TCHAR *buf, UINT buf_size)
{
    FILINFO fno;
    FRESULT fr;
    size_t len;

    if ((path == NULL) || (buf == NULL) || (buf_size < 4u))
    {
        return FR_INVALID_PARAMETER;
    }

    /* 去掉末尾的 '/'（但保留 "0:" 这种盘符形式） */
    len = strlen(path);
    while ((len > 1u) && (path[len - 1u] == '/'))
    {
        len--;
    }

    if ((len < 3u) || (path[1] != ':') || (len >= buf_size))
    {
        return FR_INVALID_NAME;     /* 空路径 / 根目录 / 太长 */
    }

    memcpy(buf, path, len);
    buf[len] = '\0';

    fr = f_stat(buf, &fno);
    if (fr != FR_OK)
    {
        return fr;
    }

    if ((fno.fattrib & AM_DIR) == 0u)
    {
        return f_unlink(buf);       /* 普通文件：直接删 */
    }

    return rm_tree(buf, len, buf_size, 0);
}
