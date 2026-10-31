/*
 * test_http_post.c - HTTP POST 请求构造的 host 单测
 *
 * 板子发给 PC 网关的请求必须是：
 *   POST /ask HTTP/1.1
 *   Host: ip:port
 *   Content-Type: text/plain; charset=utf-8
 *   Content-Length: <UTF-8 字节数，不是字符数>
 *   Connection: close
 *   <body>
 *
 * 最后一条最容易错：中文一个字符 3 字节，Content-Length 必须是字节数。
 */
#include <stdio.h>
#include <string.h>

#include "http_post.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
        {                                                                 \
            printf("FAIL %d  %s\n", __LINE__, #cond);                     \
            failures++;                                                   \
        }                                                                 \
    } while (0)

int main(void)
{
    char buf[512];
    int n;

    n = http_post_plain(buf, sizeof(buf), "192.168.1.100:8765", "/ask", "你好");
    CHECK(n > 0);
    CHECK(n == (int)strlen(buf));

    CHECK(strncmp(buf, "POST /ask HTTP/1.1\r\n", 19) == 0);
    CHECK(strstr(buf, "Host: 192.168.1.100:8765\r\n") != NULL);
    CHECK(strstr(buf, "Content-Type: text/plain; charset=utf-8\r\n") != NULL);
    CHECK(strstr(buf, "Content-Length: 6\r\n") != NULL);   /* 你好 = 6 字节 */
    CHECK(strstr(buf, "Connection: close\r\n") != NULL);
    CHECK(strstr(buf, "\r\n\r\n你好") != NULL);

    /* 空 body 也合法（Content-Length: 0） */
    n = http_post_plain(buf, sizeof(buf), "10.0.0.2:8765", "/ask", "");
    CHECK(n > 0);
    CHECK(strstr(buf, "Content-Length: 0\r\n") != NULL);

    /* 缓冲区不够 / 空指针 */
    {
        char small[24];

        CHECK(http_post_plain(small, sizeof(small), "10.0.0.2:8765", "/ask", "x") == -1);
    }
    CHECK(http_post_plain(NULL, 128, "10.0.0.2:8765", "/ask", "x") == -1);
    CHECK(http_post_plain(buf, sizeof(buf), NULL, "/ask", "x") == -1);
    CHECK(http_post_plain(buf, sizeof(buf), "10.0.0.2:8765", NULL, "x") == -1);
    CHECK(http_post_plain(buf, sizeof(buf), "10.0.0.2:8765", "/ask", NULL) == -1);

    if (failures == 0)
    {
        printf("PASS: HTTP POST 请求格式 / Content-Length / 边界全部正确\n");
        return 0;
    }
    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
