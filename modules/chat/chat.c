/*
 * chat.c - 网关配置（IP / 端口） <modules/chat/>
 *
 * 文本 chat_text.c 和语音 chat_voice.c 共用这一份配置：
 *   - 默认值编译进固件：CHAT_DEFAULT_IP / CHAT_DEFAULT_PORT；
 *   - `chat server <ip> [port]` 会写进板载 EEPROM（key: chat_ip / chat_port），
 *     掉电不丢，下次上电自动恢复；
 *   - 优先级：EEPROM > 固件默认值。
 *
 * 这里只做字符串/IP 转换和 EEPROM 读写，不涉及网络收发。
 */
#include <string.h>
#include <stdlib.h>

#include <osal.h>

#include "lwip/ip_addr.h"

#include "chat.h"
#include "eeprom_storage.h"

/* 没设置过、EEPROM 里也没有时用的默认网关（改这里重新编译即可换默认值） */
#define CHAT_DEFAULT_IP       "10.10.10.115"
#define CHAT_DEFAULT_PORT     8765u

#define CHAT_KEY_IP           "chat_ip"
#define CHAT_KEY_PORT         "chat_port"

static ip_addr_t s_server;
static uint16_t s_port = CHAT_DEFAULT_PORT;
static uint8_t s_configured;
static uint8_t s_loaded;

/* u16 → 十进制字符串（不用 snprintf，省 libc） */
static void chat_u16_str(uint16_t v, char *buf)
{
    char tmp[6];
    int i = 0;
    int j = 0;

    do
    {
        tmp[i++] = (char)('0' + (v % 10u));
        v = (uint16_t)(v / 10u);
    } while (v != 0u);

    while (i > 0)
    {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

/* 从 EEPROM 读网关；没有就用编译期默认值。只执行一次。 */
static void chat_config_load(void)
{
    eeprom_storage_t *es;
    char *ip;
    char *port;

    if (s_loaded != 0u)
    {
        return;
    }
    s_loaded = 1u;

    if (ip4addr_aton(CHAT_DEFAULT_IP, &s_server) != 0)
    {
        s_port = CHAT_DEFAULT_PORT;
        s_configured = 1u;
    }

    es = eeprom_storage_get_default();
    if (es == NULL)
    {
        return;
    }

    ip = es->get_value(es, CHAT_KEY_IP);
    port = es->get_value(es, CHAT_KEY_PORT);

    if (ip != NULL)
    {
        ip_addr_t tmp;
        int p = (port != NULL) ? atoi(port) : (int)s_port;

        if ((strlen(ip) < 16u) && (ip4addr_aton(ip, &tmp) != 0) &&
            (p > 0) && (p <= 65535))
        {
            s_server = tmp;
            s_port = (uint16_t)p;
        }
    }

    if (ip != NULL)
    {
        osal_free(ip);
    }
    if (port != NULL)
    {
        osal_free(port);
    }
}

/* 把当前网关写回 EEPROM（没有变化时不产生写操作） */
static void chat_config_save(void)
{
    eeprom_storage_t *es = eeprom_storage_get_default();
    char ipbuf[16];
    char portbuf[6];

    if (es == NULL)
    {
        osal_printf("\n[chat] 没有 EEPROM，网关只在本次运行有效\n");
        return;
    }
    if (ip4addr_ntoa_r(&s_server, ipbuf, (int)sizeof(ipbuf)) == NULL)
    {
        return;
    }
    chat_u16_str(s_port, portbuf);

    if ((es->set_value(es, CHAT_KEY_IP, ipbuf) == OSAL_OK) &&
        (es->set_value(es, CHAT_KEY_PORT, portbuf) == OSAL_OK))
    {
        (void)es->save(es);
    }
}

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
    s_loaded = 1u;
    chat_config_save();
    return CHAT_OK;
}

int chat_server_get(char *ip_buf, unsigned int ip_size, uint16_t *port)
{
    if ((ip_buf == NULL) || (ip_size < 8u))
    {
        return CHAT_ERR_PARAM;
    }

    chat_config_load();

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
