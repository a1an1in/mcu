/*
 * chat_text.c - 局域网大模型网关客户端 <modules/chat/>
 *
 * 协议极简：
 *   POST /ask  Content-Type: text/plain   body = 问题（UTF-8）
 *   响应 body = 纯文本回答（PC 网关已把 JSON 拆好）
 *
 * 用 lwIP 的 netconn API：阻塞、无需 select，和本工程“一个命令一个线程”
 * 的用法最匹配。整个请求在 msh 线程里完成，最长等 20 秒。
 */
#include <string.h>

#include <osal.h>

#include "lwip/api.h"
#include "lwip/ip_addr.h"

#include "chat.h"
#include "http_post.h"

#define CHAT_REQ_MAX          1024u
#define CHAT_CHUNK            128u    /* 单次 osal_printf 的字节数（控制台缓冲 512） */
#define CHAT_MAX_RESP         4096u   /* 最多打印这么多，避免刷屏 */
#define CHAT_RECV_TIMEOUT_MS  20000u

static ip_addr_t s_server;
static uint16_t s_port = 8765u;
static uint8_t s_configured;

static char s_req[CHAT_REQ_MAX];
static char s_host[32];
static char s_out[CHAT_CHUNK + 4u];
static size_t s_out_len;

/* ============================ 配置 ============================ */

int chat_server_set(const char *ip, uint16_t port)
{
    if ((ip == NULL) || (port == 0u))
    {
        return CHAT_ERR_PARAM;
    }
    if (ip4addr_aton(ip, &s_server) == 0)
    {
        return CHAT_ERR_PARAM;
    }
    s_port = port;
    s_configured = 1u;
    return CHAT_OK;
}

int chat_server_get(char *ip_buf, unsigned int ip_size, uint16_t *port)
{
    if ((ip_buf == NULL) || (ip_size < 8u))
    {
        return CHAT_ERR_PARAM;
    }
    if (s_configured == 0u)
    {
        return -1;
    }
    if (ip4addr_ntoa_r(&s_server, ip_buf, (int)ip_size) == NULL)
    {
        return CHAT_ERR_PARAM;
    }
    if (port != NULL)
    {
        *port = s_port;
    }
    return CHAT_OK;
}

/* ============================ 输出 ============================ */

/* 返回 s 中不超过 len 的最后一个 UTF-8 字符边界 */
static size_t utf8_cut(const char *s, size_t len)
{
    size_t i = 0;

    while (i < len)
    {
        unsigned char c = (unsigned char)s[i];
        size_t clen;

        if ((c & 0x80u) == 0u)          clen = 1u;
        else if ((c & 0xE0u) == 0xC0u)  clen = 2u;
        else if ((c & 0xF0u) == 0xE0u)  clen = 3u;
        else if ((c & 0xF8u) == 0xF0u)  clen = 4u;
        else
        {
            i++;
            continue;
        }

        if ((i + clen) > len)
        {
            break;                      /* 这个字不完整，留给下一块 */
        }
        i += clen;
    }
    return i;
}

static void chat_flush_out(void)
{
    size_t cut;

    if (s_out_len == 0u)
    {
        return;
    }

    cut = utf8_cut(s_out, s_out_len);
    if (cut == 0u)
    {
        cut = s_out_len;                /* 非法 UTF-8：原样输出 */
    }

    s_out[cut] = '\0';
    osal_printf("%s", s_out);

    if (cut < s_out_len)
    {
        memmove(s_out, &s_out[cut], s_out_len - cut);
        s_out_len -= cut;
    }
    else
    {
        s_out_len = 0u;
    }
}

static void chat_body_byte(char c)
{
    s_out[s_out_len++] = c;
    if (s_out_len >= CHAT_CHUNK)
    {
        chat_flush_out();
    }
}

/* ============================ 提问 ============================ */

int chat_text_ask(const char *text)
{
    struct netconn *conn;
    struct netbuf *nb;
    void *data;
    u16_t len;
    err_t err;
    int req_len;
    size_t n;
    unsigned int total = 0u;
    unsigned char hdr_state = 0u;
    unsigned char hdr_done = 0u;
    int truncated = 0;

    if ((text == NULL) || (text[0] == '\0'))
    {
        return CHAT_ERR_PARAM;
    }
    if (s_configured == 0u)
    {
        return CHAT_ERR_NOSERVER;
    }

    /* Host 头：ip[:port] */
    if (ip4addr_ntoa_r(&s_server, s_host, (int)sizeof(s_host)) == NULL)
    {
        return CHAT_ERR_PARAM;
    }
    n = strlen(s_host);
    if (s_port != 80u)
    {
        if ((n + 7u) >= sizeof(s_host))
        {
            return CHAT_ERR_TOOBIG;
        }
        s_host[n++] = ':';
        if (s_port >= 10000u) s_host[n++] = (char)('0' + (s_port / 10000u) % 10u);
        if (s_port >= 1000u)  s_host[n++] = (char)('0' + (s_port / 1000u) % 10u);
        s_host[n++] = (char)('0' + (s_port / 100u) % 10u);
        s_host[n++] = (char)('0' + (s_port / 10u) % 10u);
        s_host[n++] = (char)('0' + s_port % 10u);
        s_host[n] = '\0';
    }

    req_len = http_post_plain(s_req, sizeof(s_req), s_host, "/ask", text);
    if (req_len < 0)
    {
        return CHAT_ERR_TOOBIG;
    }

    conn = netconn_new(NETCONN_TCP);
    if (conn == NULL)
    {
        return CHAT_ERR_NET;
    }

    netconn_set_recvtimeout(conn, CHAT_RECV_TIMEOUT_MS);

    err = netconn_connect(conn, &s_server, s_port);
    if (err != ERR_OK)
    {
        netconn_delete(conn);
        return CHAT_ERR_NET;
    }

    err = netconn_write(conn, s_req, (size_t)req_len, NETCONN_COPY);
    if (err != ERR_OK)
    {
        netconn_close(conn);
        netconn_delete(conn);
        return CHAT_ERR_NET;
    }

    s_out_len = 0u;

    while ((err = netconn_recv(conn, &nb)) == ERR_OK)
    {
        do
        {
            netbuf_data(nb, &data, &len);
            for (u16_t i = 0; i < len; i++)
            {
                char c = ((char *)data)[i];

                if (hdr_done == 0u)
                {
                    /* 找 \r\n\r\n */
                    if (c == "\r\n\r\n"[hdr_state])
                    {
                        hdr_state++;
                        if (hdr_state == 4u)
                        {
                            hdr_done = 1u;
                        }
                    }
                    else
                    {
                        hdr_state = (c == '\r') ? 1u : 0u;
                    }
                    continue;
                }

                if (total < CHAT_MAX_RESP)
                {
                    chat_body_byte(c);
                    total++;
                    if (total == CHAT_MAX_RESP)
                    {
                        truncated = 1;
                    }
                }
            }
        } while (netbuf_next(nb) >= 0);

        netbuf_delete(nb);

        if (truncated != 0)
        {
            break;
        }
    }

    chat_flush_out();

    if (truncated != 0)
    {
        osal_printf("\n...(回答超过 %u 字节，已截断)", (unsigned)CHAT_MAX_RESP);
    }

    netconn_close(conn);
    netconn_delete(conn);
    return CHAT_OK;
}
