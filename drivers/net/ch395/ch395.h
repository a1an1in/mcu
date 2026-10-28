/*
 * ch395.h - CH395Q 以太网芯片驱动（MACRAW 模式 + lwIP）
 *
 * 硬件连接（战舰 STM32F103 V4 板载 CH395Q，SPI1）：
 *   SCK  = PA5   ┐
 *   MISO = PA6   ├ 与 VS1053、W25Q128 共用 SPI1
 *   MOSI = PA7   ┘
 *   CS   = PG9   （板上原名 FSMC_NE2）
 *   INT  = PG6   （低有效，EXTI6，板上原名 NRF_IRQ）
 *   RST  = PD7   （板上原名 RS485_RE）
 *
 * 注意：SPI1 是共享总线，本驱动在初始化时把 VS1053 按住复位
 * （PE6 = 0）并把它的片选 PF7 拉高，避免它抢 MISO。
 *
 * 传输层：ch395_port.c（SPI/GPIO/延时）
 * 命令层：ch395_cmd.c （CH395 命令码封装）
 * 网卡层：ch395_eth.c （eth_device + lwIP）
 */
#ifndef DRIVERS_NET_CH395_CH395_H__
#define DRIVERS_NET_CH395_CH395_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ CH395 命令码 ============================ */
#define CH395_CMD_GET_IC_VER        0x01
#define CH395_CMD_RESET_ALL         0x05    /* 芯片软复位 */
#define CH395_CMD_CHECK_EXIST       0x06    /* 接口自检：写 v，读回 ~v */
#define CH395_CMD_GET_GLOB_INT_ALL  0x19    /* 全局中断状态（V44 以后），读即清 */
#define CH395_CMD_SET_PHY           0x20
#define CH395_CMD_SET_MAC_ADDR      0x21
#define CH395_CMD_SET_IP_ADDR       0x22
#define CH395_CMD_SET_GWIP_ADDR     0x23
#define CH395_CMD_SET_MASK_ADDR     0x24
#define CH395_CMD_SET_MAC_FILT      0x25
#define CH395_CMD_GET_PHY_STATUS    0x26
#define CH395_CMD_INIT_CH395        0x27    /* 约 350ms，期间用 0x2C 轮询 */
#define CH395_CMD_GET_GLOB_INT      0x29
#define CH395_CMD_GET_CMD_STATUS    0x2C
#define CH395_CMD_CLEAR_RECV_BUF    0x2E
#define CH395_CMD_GET_SOCKET_STATUS 0x2F
#define CH395_CMD_GET_INT_STATUS    0x30
#define CH395_CMD_SET_IP_ADDR_SN    0x31
#define CH395_CMD_SET_DES_PORT_SN   0x32
#define CH395_CMD_SET_SOUR_PORT_SN  0x33
#define CH395_CMD_SET_PROTO_TYPE_SN 0x34
#define CH395_CMD_OPEN_SOCKET_SN    0x35
#define CH395_CMD_TCP_LISTEN_SN     0x36
#define CH395_CMD_TCP_CONNECT_SN    0x37
#define CH395_CMD_TCP_DISCONN_SN    0x38
#define CH395_CMD_WRITE_SEND_BUF_SN 0x39
#define CH395_CMD_GET_RECV_LEN_SN   0x3B
#define CH395_CMD_READ_RECV_BUF_SN  0x3C
#define CH395_CMD_CLOSE_SOCKET_SN   0x3D
#define CH395_CMD_GET_MAC_ADDR      0x40
#define CH395_CMD_DHCP_ENABLE       0x41
#define CH395_CMD_GET_DHCP_STATUS   0x42
#define CH395_CMD_GET_IP_INF        0x43
#define CH395_CMD_SET_TCP_MSS       0x50
#define CH395_CMD_SET_RECV_BUF      0x52
#define CH395_CMD_SET_SEND_BUF      0x53
#define CH395_CMD_SET_MAC_RECV_BUF  0x54

/* 命令执行状态（0x2C 返回） */
#define CH395_ERR_SUCCESS          0x00
#define CH395_ERR_BUSY             0x10

/* 协议类型（0x34 的第二个参数） */
#define CH395_PROTO_IP_RAW         0
#define CH395_PROTO_MAC_RAW        1       /* 仅 Socket0 可用 */
#define CH395_PROTO_UDP           2
#define CH395_PROTO_TCP           3

/* PHY 状态（0x26 返回） */
#define CH395_PHY_DISCONN          (1u << 0)
#define CH395_PHY_10M_FULL         (1u << 1)
#define CH395_PHY_10M_HALF         (1u << 2)
#define CH395_PHY_100M_FULL        (1u << 3)
#define CH395_PHY_100M_HALF        (1u << 4)

/* MAC 过滤位（0x25 的第一个参数） */
#define CH395_MAC_FILT_BROADCAST   (1u << 0)   /* 收广播 */
#define CH395_MAC_FILT_RECV_ALL    (1u << 1)   /* 收所有包（混杂，MACRAW 必需） */
#define CH395_MAC_FILT_MULTICAST   (1u << 2)   /* 收多播 */
#define CH395_MAC_FILT_RECV_EN     (1u << 3)   /* 使能接收 */
#define CH395_MAC_FILT_SEND_EN     (1u << 4)   /* 使能发送 */

/* 全局中断位（0x19 返回的 16 位） */
#define CH395_GINT_UNREACH         (1u << 0)
#define CH395_GINT_IP_CONFLICT     (1u << 1)
#define CH395_GINT_PHY_CHANGE      (1u << 2)
#define CH395_GINT_DHCP            (1u << 3)
#define CH395_GINT_SOCK0           (1u << 4)

/* socket 中断位（0x30 返回） */
#define CH395_SINT_SENBUF_FREE     (1u << 0)
#define CH395_SINT_SEND_OK         (1u << 1)
#define CH395_SINT_RECV            (1u << 2)
#define CH395_SINT_CONNECT         (1u << 3)
#define CH395_SINT_DISCONNECT      (1u << 4)
#define CH395_SINT_TIMEOUT         (1u << 6)

/* MACRAW 只用 Socket0；单帧最大 14 + 1500 = 1514（CRC 由芯片加） */
#define CH395_SOCK_MACRAW          0
#define CH395_MAX_FRAME            1514
#define CH395_MIN_FRAME            14

/* 传输出错/超时返回值 */
#define CH395_RET_TIMEOUT          0xFF

/* ============================ 传输层 ============================ */
/* 初始化 GPIO + SPI1（含 VS1053 隔离），并建好 SPI 事务互斥量 */
void ch395_port_init(void);

/* 硬件复位（RST 脚拉低再拉高） */
void ch395_hw_reset(void);

/* 片选：一个命令事务 = cs_low() ... cs_high()，中间不可被打断 */
void ch395_cs_low(void);
void ch395_cs_high(void);

/* 全双工交换一个字节 */
uint8_t ch395_spi_rw(uint8_t d);

/* 忙等微秒（不依赖 SysTick） */
void ch395_udelay(uint32_t us);

/* ============================ 命令层 ============================ */
uint8_t  ch395_check_exist(uint8_t v);
uint8_t  ch395_get_ic_ver(void);
uint8_t  ch395_cmd_reset_all(void);
uint8_t  ch395_cmd_status(void);
uint8_t  ch395_init_chip(void);
void     ch395_set_proto_type(uint8_t sock, uint8_t proto);
uint8_t  ch395_open_socket(uint8_t sock);
void     ch395_close_socket(uint8_t sock);
uint8_t  ch395_get_phy_status(void);
uint16_t ch395_get_glob_int_all(void);
uint8_t  ch395_get_socket_int(uint8_t sock);
uint16_t ch395_get_recv_len(uint8_t sock);
void     ch395_read_recv_buf(uint8_t sock, uint8_t *buf, uint16_t len);
void     ch395_write_send_buf(uint8_t sock, const uint8_t *buf, uint16_t len);
void     ch395_clear_recv_buf(uint8_t sock);
void     ch395_set_mac_filt(uint8_t filt, uint32_t table0, uint32_t table1);
void     ch395_get_mac(uint8_t *mac);
void     ch395_set_mac(const uint8_t *mac);

/* ============================ 网卡层 ============================ */
/* 注册 e0 到 lwIP（INIT_COMPONENT_EXPORT 自动调用） */
int ch395_netif_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_NET_CH395_CH395_H__ */
