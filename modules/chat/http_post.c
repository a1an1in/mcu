/*
 * http_post.c - 极简 HTTP/1.1 POST 请求构造 <modules/chat/>
 *
 * 不用 snprintf：本工程是 RT_USING_NANO，libc 里未必有；手工拼更可控。
 */
#include <string.h>

#include "http_post.h"

static int append_str(char **p, size_t *left, const char *s)
{
    size_t n = strlen(s);

    if ((n + 1u) > *left)
    {
        return -1;
    }
    memcpy(*p, s, n);
    *p += n;
    *left -= n;
    **p = '\0';
    return 0;
}

static int append_u32(char **p, size_t *left, unsigned int v)
{
    char tmp[11];
    int i = 0;
    int j;

    do
    {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);

    if (((size_t)i + 1u) > *left)
    {
        return -1;
    }

    for (j = 0; j < i; j++)
    {
        **p = tmp[i - 1 - j];
        (*p)++;
    }
    *left -= (size_t)i;
    **p = '\0';
    return 0;
}

int http_post_plain(char *buf, size_t size, const char *host,
                    const char *path, const char *body)
{
    char *p = buf;
    size_t left = size;
    size_t body_len;

    if ((buf == NULL) || (size < 8u) || (host == NULL) ||
        (path == NULL) || (body == NULL))
    {
        return -1;
    }

    body_len = strlen(body);

    if (append_str(&p, &left, "POST ") != 0) return -1;
    if (append_str(&p, &left, path) != 0) return -1;
    if (append_str(&p, &left, " HTTP/1.1\r\nHost: ") != 0) return -1;
    if (append_str(&p, &left, host) != 0) return -1;
    if (append_str(&p, &left, "\r\nContent-Type: text/plain; charset=utf-8\r\n"
                             "Content-Length: ") != 0) return -1;
    if (append_u32(&p, &left, (unsigned int)body_len) != 0) return -1;
    if (append_str(&p, &left, "\r\nConnection: close\r\n\r\n") != 0) return -1;
    if (append_str(&p, &left, body) != 0) return -1;

    return (int)(p - buf);
}
