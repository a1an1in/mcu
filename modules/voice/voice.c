/*
 * voice.c - 流式语音对话（VS1053 录音 → TCP → ASR/LLM/TTS → VS1053 播放）
 *
 * 设计目标是把「录完再传」的 5~10 秒延迟压到 2~4 秒：
 *   - 录音时同步 512B 发送，不写 SD；
 *   - 松键立即半关闭 TCP，PC 不用等文件/长度；
 *   - 收到回答 WAV 后边收边喂 VS1053，不等整个文件下载完。
 *
 * HTTP 用 1.0 且不带 Content-Length：发完 PCM 后 shutdown(TX)，PC 读到 EOF
 * 就知道说完了 —— 比在 MCU 上实现 chunked 简单。
 */
#include <string.h>

#include <osal.h>

#include "lwip/api.h"
#include "lwip/ip_addr.h"

#include "ai.h"
#include "key.h"
#include "music.h"
#include "recorder.h"
#include "vs1053.h"
#include "voice.h"

#define VOICE_KEY           KEY_0
#define VOICE_RATE          8000u
#define VOICE_GAIN_REG      4096u       /* MIC 4 倍增益 */
#define VOICE_BUF_SIZE      512u
#define VOICE_MAX_SECONDS   30u
#define VOICE_MAX_BYTES     (VOICE_RATE * 2u * VOICE_MAX_SECONDS)
#define VOICE_STACK_SIZE    4096u
#define VOICE_PRIORITY      14u
#define VOICE_RECV_TO_MS    60000u      /* ASR + LLM + TTS 最长等 60s */

static osal_thread_t s_thread;
static volatile uint8_t s_enabled = 1u;
static volatile uint8_t s_busy;
static volatile uint8_t s_recording;
static uint8_t s_pcm[VOICE_BUF_SIZE] __attribute__((aligned(4)));

/* ============================ HTTP 头 ============================ */

static int voice_build_header(char *buf, size_t size, const char *ip, uint16_t port)
{
    char portstr[6];
    int i;

    for (i = 4; i >= 0; i--)
    {
        portstr[i] = (char)('0' + (port % 10u));
        port /= 10u;
    }
    portstr[5] = '\0';

    if ((strlen(ip) + 150u) >= size)
    {
        return -1;
    }

    buf[0] = '\0';
    strcat(buf, "POST /voice HTTP/1.0\r\nHost: ");
    strcat(buf, ip);
    strcat(buf, ":");
    strcat(buf, portstr);
    strcat(buf, "\r\nContent-Type: application/octet-stream\r\n");
    strcat(buf, "X-Sample-Rate: 8000\r\nX-Channels: 1\r\nX-Bits: 16\r\n\r\n");
    return (int)strlen(buf);
}

/* ============================ 读回答 ============================ */

static void voice_read_reply(struct netconn *conn)
{
    struct vs1053 *dev = vs1053_get_default();
    struct netbuf *nb;
    void *data;
    u16_t len;
    char hdr[512];
    size_t hlen = 0;
    char txt[200];
    size_t tlen = 0;
    uint8_t abuf[128];
    size_t alen = 0;
    uint8_t hdr_done = 0;
    uint8_t is_audio = 0;

    tlen = 0;
    txt[0] = '\0';

    while (netconn_recv(conn, &nb) == ERR_OK)
    {
        do
        {
            u16_t i;

            netbuf_data(nb, &data, &len);
            for (i = 0; i < len; i++)
            {
                uint8_t c = ((uint8_t *)data)[i];

                if (hdr_done == 0u)
                {
                    if (hlen < (sizeof(hdr) - 1u))
                    {
                        hdr[hlen++] = (char)c;
                        hdr[hlen] = '\0';
                    }
                    if ((hlen >= 4u) &&
                        (memcmp(&hdr[hlen - 4u], "\r\n\r\n", 4) == 0))
                    {
                        hdr_done = 1u;
                        is_audio = (strstr(hdr, "Content-Type: audio/") != NULL) ? 1u : 0u;
                        if (is_audio != 0u)
                        {
                            osal_printf("\n[voice] 收到回答，边下边播...\n");
                        }
                    }
                    continue;
                }

                if (is_audio != 0u)
                {
                    abuf[alen++] = c;
                    if (alen >= sizeof(abuf))
                    {
                        (void)vs1053_write_data(dev, abuf, (uint16_t)alen);
                        alen = 0;
                    }
                }
                else if (tlen < (sizeof(txt) - 1u))
                {
                    txt[tlen++] = (char)c;
                    txt[tlen] = '\0';
                }
            }
        } while (netbuf_next(nb) >= 0);

        netbuf_delete(nb);
    }

    if (is_audio != 0u)
    {
        if (alen != 0u)
        {
            (void)vs1053_write_data(dev, abuf, (uint16_t)alen);
        }
    }
    else
    {
        osal_printf("\n[voice] 服务器返回: %s\n", txt);
    }
}

/* ============================ 一轮对话 ============================ */

static void voice_cycle(void)
{
    struct vs1053 *dev = vs1053_get_default();
    struct netconn *conn = NULL;
    ip_addr_t addr;
    char ip[24];
    uint16_t port = 0;
    char hdr[256];
    int hlen;
    uint32_t sent = 0;

    if (dev == NULL)
    {
        osal_printf("\n[voice] 没有 VS1053\n");
        return;
    }
    if (recorder_is_recording() != 0)
    {
        osal_printf("\n[voice] 正在 record，先 record stop\n");
        return;
    }
    if (ai_get_server(ip, sizeof(ip), &port) != AI_OK)
    {
        osal_printf("\n[voice] 先 ai server <PC_IP> [port]\n");
        return;
    }
    if (ip4addr_aton(ip, &addr) == 0)
    {
        osal_printf("\n[voice] 网关 IP 非法: %s\n", ip);
        return;
    }
    if (music_is_playing() != 0)
    {
        (void)music_stop();
    }

    conn = netconn_new(NETCONN_TCP);
    if (conn == NULL)
    {
        osal_printf("\n[voice] 内存不足，建 TCP 失败\n");
        return;
    }
    netconn_set_recvtimeout(conn, VOICE_RECV_TO_MS);

    if (netconn_connect(conn, &addr, port) != ERR_OK)
    {
        osal_printf("\n[voice] 连不上网关 %s:%u\n", ip, (unsigned)port);
        netconn_delete(conn);
        return;
    }

    hlen = voice_build_header(hdr, sizeof(hdr), ip, port);
    if ((hlen < 0) ||
        (netconn_write(conn, hdr, (size_t)hlen, NETCONN_COPY) != ERR_OK))
    {
        osal_printf("\n[voice] 发请求头失败\n");
        netconn_close(conn);
        netconn_delete(conn);
        return;
    }

    if (vs1053_record_start(dev, VOICE_RATE, VOICE_GAIN_REG) != VS1053_OK)
    {
        osal_printf("\n[voice] VS1053 进入录音失败\n");
        netconn_close(conn);
        netconn_delete(conn);
        return;
    }

    s_recording = 1u;
    osal_printf("\n[voice] 按住 KEY0 说话，松开结束...\n");

    while ((key_is_down(VOICE_KEY) != 0) && (sent < VOICE_MAX_BYTES))
    {
        if (vs1053_record_read(dev, s_pcm, VOICE_BUF_SIZE) != VS1053_OK)
        {
            osal_printf("\n[voice] 录音 FIFO 超时\n");
            break;
        }
        if (netconn_write(conn, s_pcm, VOICE_BUF_SIZE, NETCONN_COPY) != ERR_OK)
        {
            osal_printf("\n[voice] 网络发送失败\n");
            break;
        }
        sent += VOICE_BUF_SIZE;
    }

    s_recording = 0u;

    /* 半关闭发送：PC 读到 EOF 就开始 ASR */
    (void)netconn_shutdown(conn, 0, 1);

    /* 与 PC 处理并行：把 VS1053 从录音切回播放 */
    (void)vs1053_record_stop(dev);

    osal_printf("\n[voice] 已发送 %u 字节，等待识别/回答...\n", (unsigned)sent);

    voice_read_reply(conn);

    netconn_close(conn);
    netconn_delete(conn);
}

/* ============================ 按键线程 ============================ */

static void voice_thread(void *param)
{
    int last = 1;   /* 启动时若按键已按下，先等松开再触发 */

    (void)param;

    for (;;)
    {
        int down = key_is_down(VOICE_KEY);

        if ((s_enabled != 0u) && (s_busy == 0u) && (down != 0) && (last == 0))
        {
            s_busy = 1u;
            voice_cycle();
            s_busy = 0u;

            /* 等按键松开，避免一次长按触发第二轮 */
            while (key_is_down(VOICE_KEY) != 0)
            {
                osal_thread_sleep_ms(20);
            }
            last = 0;
            continue;
        }

        last = down;
        osal_thread_sleep_ms(20);
    }
}

int voice_service_start(void)
{
    if (s_thread != OSAL_NULL)
    {
        return 0;
    }

    key_init();     /* 幂等 */

    if (osal_thread_create(&s_thread, "voice", voice_thread, NULL,
                           NULL, VOICE_STACK_SIZE, VOICE_PRIORITY) != OSAL_OK)
    {
        s_thread = OSAL_NULL;
        return -1;
    }
    return 0;
}

void voice_key_enable(int on)
{
    s_enabled = (on != 0) ? 1u : 0u;
}

int voice_key_enabled(void)
{
    return (s_enabled != 0u) ? 1 : 0;
}

int voice_is_busy(void)
{
    return (s_busy != 0u) ? 1 : 0;
}

int voice_is_recording(void)
{
    return (s_recording != 0u) ? 1 : 0;
}
