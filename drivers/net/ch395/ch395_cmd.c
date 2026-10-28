/*
 * ch395_cmd.c - CH395Q 命令层
 *
 * 时序严格按 WCH 官方 EVT（CH395SPI_HW.C / CH395CMD.C）和正点原子
 * ch395cmd.c 移植：
 *   1. CS 拉低
 *   2. 发命令码，延时 >= 1.5us
 *   3. 发/收数据
 *   4. CS 拉高，延时 >= 1.5us（保证两次命令之间有空隙）
 *
 * 需要等待执行结果的命令（INIT / OPEN）用 0x2C 轮询，
 * 返回 0x00 = 成功，0x10 = 忙。
 */
#include <rtthread.h>

#include "ch395.h"

/* ============================ 事务原语 ============================ */
static void ch395_write_cmd(uint8_t cmd)
{
    ch395_cs_low();
    ch395_spi_rw(cmd);
    ch395_udelay(2);
}

static void ch395_write_data(uint8_t d)
{
    ch395_spi_rw(d);
}

static uint8_t ch395_read_data(void)
{
    return ch395_spi_rw(0xFF);
}

static void ch395_end_cmd(void)
{
    ch395_cs_high();
    ch395_udelay(2);
}

/* ============================ 基础命令 ============================ */
uint8_t ch395_check_exist(uint8_t v)
{
    uint8_t r;

    ch395_write_cmd(CH395_CMD_CHECK_EXIST);
    ch395_write_data(v);
    ch395_udelay(1);
    r = ch395_read_data();
    ch395_end_cmd();

    return r;   /* 正常应返回 (uint8_t)~v */
}

uint8_t ch395_cmd_reset_all(void)
{
    ch395_write_cmd(CH395_CMD_RESET_ALL);
    ch395_end_cmd();

    /* 软复位后芯片要重新初始化，这段时间不能再发其它命令 */
    rt_thread_mdelay(100);

    return ch395_cmd_status();
}

uint8_t ch395_cmd_status(void)
{
    uint8_t s;

    ch395_write_cmd(CH395_CMD_GET_CMD_STATUS);
    s = ch395_read_data();
    ch395_end_cmd();

    return s;
}

/* 初始化芯片：命令发出后芯片忙约 350ms，期间不能发别的命令，
 * 只能每 10ms 查一次状态（官方建议 >= 2ms）。 */
uint8_t ch395_init_chip(void)
{
    int i;
    uint8_t s;

    ch395_write_cmd(CH395_CMD_INIT_CH395);
    ch395_end_cmd();

    for (i = 0; i < 250; i++)          /* 上限 2.5s */
    {
        rt_thread_mdelay(10);
        s = ch395_cmd_status();
        if (s != CH395_ERR_BUSY)
        {
            return s;
        }
    }

    return CH395_RET_TIMEOUT;
}

/* ============================ Socket 配置 ============================ */
void ch395_set_proto_type(uint8_t sock, uint8_t proto)
{
    ch395_write_cmd(CH395_CMD_SET_PROTO_TYPE_SN);
    ch395_write_data(sock);
    ch395_write_data(proto);
    ch395_end_cmd();
}

uint8_t ch395_open_socket(uint8_t sock)
{
    int i;
    uint8_t s;

    ch395_write_cmd(CH395_CMD_OPEN_SOCKET_SN);
    ch395_write_data(sock);
    ch395_end_cmd();

    for (i = 0; i < 200; i++)          /* 上限 1s */
    {
        rt_thread_mdelay(5);
        s = ch395_cmd_status();
        if (s != CH395_ERR_BUSY)
        {
            return s;
        }
    }

    return CH395_RET_TIMEOUT;
}

void ch395_close_socket(uint8_t sock)
{
    ch395_write_cmd(CH395_CMD_CLOSE_SOCKET_SN);
    ch395_write_data(sock);
    ch395_end_cmd();
}

/* ============================ 状态查询 ============================ */
uint8_t ch395_get_ic_ver(void)
{
    uint8_t v;

    ch395_write_cmd(CH395_CMD_GET_IC_VER);
    v = ch395_read_data();
    ch395_end_cmd();

    return v;
}

uint8_t ch395_get_phy_status(void)
{
    uint8_t v;

    ch395_write_cmd(CH395_CMD_GET_PHY_STATUS);
    v = ch395_read_data();
    ch395_end_cmd();

    return v;
}

uint16_t ch395_get_glob_int_all(void)
{
    uint16_t v;

    /* 读这个命令会同时把 CH395 的 INT 引脚释放（手册 5.2.3） */
    ch395_write_cmd(CH395_CMD_GET_GLOB_INT_ALL);
    v  = ch395_read_data();
    v |= (uint16_t)ch395_read_data() << 8;
    ch395_end_cmd();

    return v;
}

uint8_t ch395_get_socket_int(uint8_t sock)
{
    uint8_t v;

    ch395_write_cmd(CH395_CMD_GET_INT_STATUS);
    ch395_write_data(sock);
    ch395_udelay(2);
    v = ch395_read_data();
    ch395_end_cmd();

    return v;
}

/* ============================ 收发缓冲 ============================ */
uint16_t ch395_get_recv_len(uint8_t sock)
{
    uint16_t len;

    ch395_write_cmd(CH395_CMD_GET_RECV_LEN_SN);
    ch395_write_data(sock);
    len  = ch395_read_data();
    len |= (uint16_t)ch395_read_data() << 8;
    ch395_end_cmd();

    return len;
}

void ch395_read_recv_buf(uint8_t sock, uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (len == 0u)
    {
        return;
    }

    ch395_write_cmd(CH395_CMD_READ_RECV_BUF_SN);
    ch395_write_data(sock);
    ch395_write_data((uint8_t)len);
    ch395_write_data((uint8_t)(len >> 8));
    ch395_udelay(1);

    for (i = 0; i < len; i++)
    {
        buf[i] = ch395_read_data();
    }

    ch395_end_cmd();
}

void ch395_write_send_buf(uint8_t sock, const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (len == 0u)
    {
        return;
    }

    ch395_write_cmd(CH395_CMD_WRITE_SEND_BUF_SN);
    ch395_write_data(sock);
    ch395_write_data((uint8_t)len);
    ch395_write_data((uint8_t)(len >> 8));

    for (i = 0; i < len; i++)
    {
        ch395_write_data(buf[i]);
    }

    ch395_end_cmd();
}

void ch395_clear_recv_buf(uint8_t sock)
{
    ch395_write_cmd(CH395_CMD_CLEAR_RECV_BUF);
    ch395_write_data(sock);
    ch395_end_cmd();
}

/* ============================ MAC ============================ */
void ch395_set_mac_filt(uint8_t filt, uint32_t table0, uint32_t table1)
{
    uint8_t i;

    ch395_write_cmd(CH395_CMD_SET_MAC_FILT);
    ch395_write_data(filt);

    for (i = 0; i < 4; i++)
    {
        ch395_write_data((uint8_t)(table0 >> (8u * i)));
    }
    for (i = 0; i < 4; i++)
    {
        ch395_write_data((uint8_t)(table1 >> (8u * i)));
    }

    ch395_end_cmd();
}

void ch395_get_mac(uint8_t *mac)
{
    uint8_t i;

    ch395_write_cmd(CH395_CMD_GET_MAC_ADDR);
    for (i = 0; i < 6; i++)
    {
        mac[i] = ch395_read_data();
    }
    ch395_end_cmd();
}

void ch395_set_mac(const uint8_t *mac)
{
    uint8_t i;

    /* 手册要求：SET_MAC_ADDR 必须在 INIT_CH395 之前执行 */
    ch395_write_cmd(CH395_CMD_SET_MAC_ADDR);
    for (i = 0; i < 6; i++)
    {
        ch395_write_data(mac[i]);
    }
    ch395_end_cmd();

    rt_thread_mdelay(10);
}
