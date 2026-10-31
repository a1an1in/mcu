/*
 * http_post.h - 极简 HTTP/1.1 POST 请求构造 <modules/ai/>
 *
 * 只拼「POST + Host + Content-Length + 明文 body」，不碰 socket。
 * 单独成文件是为了能在宿主机上单测字节格式。
 */
#ifndef MODULES_AI_HTTP_POST_H__
#define MODULES_AI_HTTP_POST_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 生成：
 *   POST <path> HTTP/1.1
 *   Host: <host>
 *   Content-Type: text/plain; charset=utf-8
 *   Content-Length: <body 字节数>
 *   Connection: close
 *
 * 写入 buf（含结尾 '\0'），返回总字节数；失败返回 -1。
 * host 形如 "192.168.1.100:8765"。 */
int http_post_plain(char *buf, size_t size, const char *host,
                    const char *path, const char *body);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_AI_HTTP_POST_H__ */
