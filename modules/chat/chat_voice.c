/*
 * chat_voice.c - 流式语音对话（VS1053 录音 → TCP → ASR/LLM/TTS → VS1053 播放）
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

#include "chat.h"
#include "key.h"
#include "music.h"
#include "recorder.h"
#include "vs1053.h"

#define VOICE_KEY           KEY_0
#define VOICE_RATE          8000u
#define VOICE_GAIN_DEFAULT  4096u       /* MIC 默认 4 倍增益 */
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
static uint16_t s_gain_reg = VOICE_GAIN_DEFAULT;
static uint8_t s_pcm[VOICE_BUF_SIZE] __attribute__((aligned(4)));
/* 网关在音频前附带的一小段对话文字（你: ... / AI: ...），上限 1900 字节 */
static char s_text[2048];
static size_t s_text_len;

/* osal_printf 的格式化缓冲只有 RT_CONSOLEBUF_SIZE(512) 字节，
 * 长文本一次打印会被截断，所以按 UTF-8 边界分段输出。 */
static void voice_print_text(void)
{
    size_t off = 0u;

    osal_printf("\n");
    while (off < s_text_len)
    {
        char tmp[256];
        size_t n = s_text_len - off;

        if (n > (sizeof(tmp) - 1u))
        {
            n = sizeof(tmp) - 1u;
        }
        while ((n > 0u) &&
               (((unsigned char)s_text[off + n] & 0xC0u) == 0x80u))
        {
            n--;                    /* 不要把 UTF-8 字符切一半 */
        }
        if (n == 0u)
        {
            break;
        }
        memcpy(tmp, &s_text[off], n);
        tmp[n] = '\0';
        osal_printf("%s", tmp);
        off += n;
    }
}

/* 从响应头里取一个十进制整数（找不到返回 0） */
static uint32_t voice_header_u32(const char *hdr, const char *name)
{
    const char *p = strstr(hdr, name);
    uint32_t v = 0u;

    if (p == NULL)
    {
        return 0u;
    }
    p += strlen(name);
    while ((*p == ' ') || (*p == '\t'))
    {
        p++;
    }
    while ((*p >= '0') && (*p <= '9'))
    {
        v = (v * 10u) + (uint32_t)(*p - '0');
        p++;
    }
    return v;
}

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
    strcat(buf, "X-Sample-Rate: 8000\r\nX-Channels: 1\r\nX-Bits: 16\r\n");
    strcat(buf, "X-Want-Text: 1\r\n\r\n");
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
    uint32_t text_left = 0u;
    uint32_t fed = 0u;
    uint32_t werr = 0u;

    tlen = 0;
    txt[0] = '\0';
    s_text_len = 0u;

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
                        text_left = voice_header_u32(hdr, "X-Text-Length:");
                        if (is_audio != 0u)
                        {
                            osal_printf("\n[voice] 收到回答，边下边播...\n");
                        }
                    }
                    continue;
                }

                /* 音频前面是对话文字：先收满并打印，后面的字节再喂 VS1053 */
                if (text_left > 0u)
                {
                    if (s_text_len < (sizeof(s_text) - 1u))
                    {
                        s_text[s_text_len++] = (char)c;
                    }
                    text_left--;
                    if (text_left == 0u)
                    {
                        s_text[s_text_len] = '\0';
                        voice_print_text();
                    }
                    continue;
                }

                if (is_audio != 0u)
                {
                    abuf[alen++] = c;
                    if (alen >= sizeof(abuf))
                    {
                        if (vs1053_write_data(dev, abuf, (uint16_t)alen) != VS1053_OK)
                        {
                            werr++;
                        }
                        fed += (uint32_t)alen;
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
            if (vs1053_write_data(dev, abuf, (uint16_t)alen) != VS1053_OK)
            {
                werr++;
            }
            fed += (uint32_t)alen;
        }
        osal_printf("\n[voice] 音频 %u 字节，写失败 %u 次，ready=%d\n",
                    (unsigned)fed, (unsigned)werr, (int)dev->ready);
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
    if (chat_server_get(ip, sizeof(ip), &port) != CHAT_OK)
    {
        osal_printf("\n[voice] 先 chat server <PC_IP> [port]\n");
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

    if (vs1053_record_start(dev, VOICE_RATE, s_gain_reg) != VS1053_OK)
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
    {
        int rs = vs1053_record_stop(dev);

        if (rs != VS1053_OK)
        {
            osal_printf("\n[voice] 录音切回播放失败(%d)，重试初始化\n", rs);
            (void)vs1053_init(dev);
        }
    }

    /* 录音切回播放会把音量重置成默认 80；这里恢复 music/voice vol 的值 */
    (void)vs1053_set_volume(dev, music_get_volume());
    osal_printf("\n[voice] 播放准备: ready=%d 音量=%d\n",
                (int)dev->ready, music_get_volume());

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

int chat_voice_service_start(void)
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

void chat_voice_key_enable(int on)
{
    s_enabled = (on != 0) ? 1u : 0u;
}

int chat_chat_voice_key_enabled(void)
{
    return (s_enabled != 0u) ? 1 : 0;
}

int chat_voice_is_busy(void)
{
    return (s_busy != 0u) ? 1 : 0;
}

int chat_voice_is_recording(void)
{
    return (s_recording != 0u) ? 1 : 0;
}

void chat_voice_set_gain(int gain)
{
    if (gain <= 0)
    {
        s_gain_reg = 0u;                /* 0 = 自动 AGC */
    }
    else if (gain > 15)
    {
        s_gain_reg = (uint16_t)(15u * 1024u);
    }
    else
    {
        s_gain_reg = (uint16_t)((unsigned)gain * 1024u);
    }
}

int chat_voice_get_gain(void)
{
    return (s_gain_reg == 0u) ? 0 : (int)(s_gain_reg / 1024u);
}
