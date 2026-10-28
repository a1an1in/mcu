/*
 * ch395_eth.c - CH395Q 的 lwIP 网卡层（eth_device + MACRAW）
 *
 * 数据通路：
 *   CH395 INT(PG6) 下降沿 → EXTI9_5 → eth_device_ready() 唤醒 erx 线程
 *   erx 线程 → ch395_eth_rx()：读全局中断 → 读 socket 中断 → 读一帧 → pbuf
 *   lwIP 发帧 → etx 线程 → ch395_eth_tx()：写发送缓冲 → 芯片自动加 CRC 发出
 *
 * 不在中断里碰 SPI：中断只清 EXTI 挂起并投递邮箱，所有 SPI 事务都在线程里做，
 * 这样既不用在 ISR 里关中断（一帧 1514 字节在 4MHz SPI 上约 3ms），
 * 也避免了和 erx/etx 抢总线。
 */
#include <rtthread.h>
#include <netif/ethernetif.h>
#include <lwip/pbuf.h>

#include "ch395.h"

/* ============================ 中断相关寄存器 ============================ */
#define EXTI_BASE       0x40010400u
#define EXTI_IMR        (*(volatile uint32_t *)(EXTI_BASE + 0x00u))
#define EXTI_RTSR       (*(volatile uint32_t *)(EXTI_BASE + 0x08u))
#define EXTI_FTSR       (*(volatile uint32_t *)(EXTI_BASE + 0x0Cu))
#define EXTI_PR         (*(volatile uint32_t *)(EXTI_BASE + 0x14u))

#define AFIO_EXTICR2    (*(volatile uint32_t *)(0x40010000u + 0x0Cu))
#define NVIC_ISER0      (*(volatile uint32_t *)(0xE000E100u))

#define GPIOG_BASE      0x40012000u
#define GPIO_IDR(b)     (*(volatile uint32_t *)((b) + 0x08u))

#define CH395_INT_LINE  6u

/* ============================ 静态数据 ============================ */
#define CH395_RX_BUF_SIZE   1536

static struct eth_device s_dev;
static uint8_t  s_mac[6];
static uint8_t  s_rx_buf[CH395_RX_BUF_SIZE];   /* 丢弃帧/链式 pbuf 的临时缓冲 */
static uint8_t  s_tx_buf[CH395_RX_BUF_SIZE];   /* 仅在 pbuf 链式时用 */
static rt_bool_t s_link_up;
static rt_bool_t s_ready;

/* 收发计数，仅在 ch395 命令里打印，用于区分「帧没到驱动」和「到了但协议栈不理」 */
static uint32_t  s_rx_frames;
static uint32_t  s_rx_drop;
static uint32_t  s_tx_frames;

/* 最近几帧的头（目的 MAC / 源 MAC / 类型 / ARP 操作码或 IP 协议 / IP 对），
 * 用 msh > ch395 log 打印。排查「DHCP 通但 ping 不通」时非常直接：
 *   rx 里能看到 ARP reply 且 src 是网关 MAC → 应答到了，问题在协议栈/上层；
 *   rx 里始终没有 ARP reply → 响应根本没到驱动。
 *   tx 里没有 ARP request → 协议栈压根没发（netif 配置有问题）。 */
#define CH395_FRAME_LOG_NUM  6

struct ch395_frame_hdr
{
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
    uint16_t len;
    uint32_t sip;     /* ARP: sender IP；IPv4: 源 IP（网络字节序） */
    uint32_t dip;     /* ARP: target IP；IPv4: 目的 IP */
    uint8_t  proto;   /* ARP: 操作码 1=请求 2=应答；IPv4: 1=ICMP 6=TCP 17=UDP */
};

static struct ch395_frame_hdr s_rx_log[CH395_FRAME_LOG_NUM];
static struct ch395_frame_hdr s_tx_log[CH395_FRAME_LOG_NUM];
static uint32_t s_rx_log_idx;
static uint32_t s_tx_log_idx;

static void ch395_log_frame(struct ch395_frame_hdr *log, uint32_t *idx,
                            const uint8_t *buf, uint16_t len)
{
    struct ch395_frame_hdr *e;

    if (len < 14u)
    {
        return;
    }

    e = &log[*idx];
    rt_memcpy(e->dst, buf, 6);
    rt_memcpy(e->src, buf + 6, 6);
    e->type  = (uint16_t)(((uint16_t)buf[12] << 8) | buf[13]);
    e->len   = len;
    e->sip   = 0;
    e->dip   = 0;
    e->proto = 0;

    if (e->type == 0x0806u && len >= 42u)
    {
        e->proto = (uint8_t)(((uint16_t)buf[20] << 8) | buf[21]);   /* opcode */
        rt_memcpy(&e->sip, buf + 28, 4);
        rt_memcpy(&e->dip, buf + 38, 4);
    }
    else if (e->type == 0x0800u && len >= 34u)
    {
        e->proto = buf[23];                                        /* 协议号 */
        rt_memcpy(&e->sip, buf + 26, 4);
        rt_memcpy(&e->dip, buf + 30, 4);
    }

    *idx = (*idx + 1u) % CH395_FRAME_LOG_NUM;
}

/* ============================ 中断 ============================ */
static void ch395_exti_init(void)
{
    /* EXTI6 映射到 GPIOG（AFIO_EXTICR2 的 bit11:8 = 6） */
    AFIO_EXTICR2 = (AFIO_EXTICR2 & ~(0xFu << 8)) | (0x6u << 8);

    EXTI_FTSR |= (1u << CH395_INT_LINE);    /* 下降沿（CH395 INT 低有效） */
    EXTI_RTSR &= ~(1u << CH395_INT_LINE);
    EXTI_PR    = (1u << CH395_INT_LINE);    /* 写 1 清挂起 */
    EXTI_IMR  |= (1u << CH395_INT_LINE);

    NVIC_ISER0 = (1u << 23);                /* IRQ23 = EXTI9_5 */
}

/* IRQ23：EXTI5..EXTI9 共用 */
void EXTI9_5_IRQHandler(void)
{
    rt_interrupt_enter();

    if (EXTI_PR & (1u << CH395_INT_LINE))
    {
        EXTI_PR = (1u << CH395_INT_LINE);

        /* 只投递邮箱，不碰 SPI；芯片 INT 要等 erx 线程读完全局中断才释放 */
        if (s_dev.netif != RT_NULL)
        {
            eth_device_ready(&s_dev);
        }
    }

    rt_interrupt_leave();
}

/* ============================ 链路状态 ============================ */
static void ch395_update_link(void)
{
    uint8_t phy = ch395_get_phy_status();
    rt_bool_t up;

    up = ((phy & CH395_PHY_DISCONN) == 0u && phy != 0u) ? RT_TRUE : RT_FALSE;

    if (up == s_link_up)
    {
        return;
    }
    s_link_up = up;

    eth_device_linkchange(&s_dev, up);

    if (up)
    {
        const char *speed = "unknown";
        if (phy & CH395_PHY_100M_FULL)      speed = "100M full";
        else if (phy & CH395_PHY_100M_HALF) speed = "100M half";
        else if (phy & CH395_PHY_10M_FULL)  speed = "10M full";
        else if (phy & CH395_PHY_10M_HALF)  speed = "10M half";

        rt_kprintf("[ch395] link up, %s\n", speed);
    }
    else
    {
        rt_kprintf("[ch395] link down\n");
    }
}

/* ============================ 收包 ============================ */
static struct pbuf *ch395_eth_rx(rt_device_t dev)
{
    struct pbuf *p;
    uint16_t gint;
    uint16_t len;
    uint8_t  sint;

    (void)dev;

    /* 读全局中断会同时释放 CH395 的 INT 引脚 */
    gint = ch395_get_glob_int_all();

    if (gint & CH395_GINT_PHY_CHANGE)
    {
        ch395_update_link();
    }

    if ((gint & CH395_GINT_SOCK0) == 0u)
    {
        return RT_NULL;
    }

    sint = ch395_get_socket_int(CH395_SOCK_MACRAW);

    if ((sint & CH395_SINT_RECV) == 0u)
    {
        return RT_NULL;
    }

    len = ch395_get_recv_len(CH395_SOCK_MACRAW);

    /* MACRAW 一帧 = 14 字节头 + 46~1500 数据；越界说明状态异常，清掉缓冲 */
    if (len < CH395_MIN_FRAME || len > CH395_MAX_FRAME)
    {
        s_rx_drop++;
        if (len != 0u)
        {
            ch395_clear_recv_buf(CH395_SOCK_MACRAW);
        }
        return RT_NULL;
    }

    p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p == RT_NULL)
    {
        /* 拿不到 pbuf 也必须把芯片缓冲读空，否则下一帧永远收不到 */
        s_rx_drop++;
        ch395_read_recv_buf(CH395_SOCK_MACRAW, s_rx_buf, len);
        return RT_NULL;
    }

    if (p->next == RT_NULL && p->len >= len)
    {
        ch395_read_recv_buf(CH395_SOCK_MACRAW, (uint8_t *)p->payload, len);
        ch395_log_frame(s_rx_log, &s_rx_log_idx, (const uint8_t *)p->payload, len);
    }
    else
    {
        ch395_read_recv_buf(CH395_SOCK_MACRAW, s_rx_buf, len);
        ch395_log_frame(s_rx_log, &s_rx_log_idx, s_rx_buf, len);
        pbuf_take(p, s_rx_buf, len);
    }

    s_rx_frames++;

    return p;
}

/* ============================ 发包 ============================ */
/* 以太网最小帧 60 字节（6+6+2 头 + 46 载荷，不含 4 字节 FCS）。
 * CH395 的 MACRAW 不做任何帧处理，短帧不补零：
 *   ARP 帧只有 42 字节，发到线上会被交换机/网卡当 runt 丢弃，
 *   现象就是「板子明明回了 ARP 应答，对方却一直说自己 ARP 不通」、
 *   「DHCP（长帧）正常，ping（先要 ARP）全超时」。 */
#define CH395_MIN_TX_FRAME  60u

static rt_err_t ch395_eth_tx(rt_device_t dev, struct pbuf *p)
{
    const uint8_t *frame;
    uint16_t len;
    uint16_t tx_len;

    (void)dev;

    if (p == RT_NULL || p->tot_len == 0u || p->tot_len > CH395_MAX_FRAME)
    {
        return -RT_EIO;
    }

    len    = p->tot_len;
    tx_len = (len < CH395_MIN_TX_FRAME) ? (uint16_t)CH395_MIN_TX_FRAME : len;

    if (p->next == RT_NULL && tx_len == len)
    {
        /* 单 pbuf：载荷本身就是连续的，直接发，省一次 1.5KB 拷贝 */
        frame = (const uint8_t *)p->payload;
    }
    else
    {
        /* 链式 pbuf 或者需要补零到最小帧 */
        rt_memset(s_tx_buf, 0, tx_len);
        pbuf_copy_partial(p, s_tx_buf, len, 0);
        frame = s_tx_buf;
    }

    ch395_write_send_buf(CH395_SOCK_MACRAW, frame, tx_len);
    ch395_log_frame(s_tx_log, &s_tx_log_idx, frame, tx_len);

    s_tx_frames++;

    return RT_EOK;
}

/* ============================ rt_device 回调 ============================ */
static rt_err_t ch395_eth_dev_init(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

static rt_err_t ch395_eth_dev_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;
    return RT_EOK;
}

static rt_err_t ch395_eth_dev_close(rt_device_t dev)
{
    (void)dev;
    ch395_close_socket(CH395_SOCK_MACRAW);
    return RT_EOK;
}

static rt_err_t ch395_eth_dev_control(rt_device_t dev, int cmd, void *args)
{
    (void)dev;

    switch (cmd)
    {
    case NIOCTL_GADDR:                      /* 把 MAC 交给 lwIP 填 netif->hwaddr */
        if (args == RT_NULL)
        {
            return -RT_ERROR;
        }
        rt_memcpy(args, s_mac, 6);
        return RT_EOK;

    default:
        return -RT_ENOSYS;
    }
}

/* ============================ 芯片初始化 ============================ */
static rt_bool_t ch395_mac_valid(const uint8_t *mac)
{
    int i;
    rt_bool_t all_zero = RT_TRUE;
    rt_bool_t all_ff   = RT_TRUE;

    if (mac[0] & 0x01u)                     /* 组播位不能置 1 */
    {
        return RT_FALSE;
    }

    for (i = 0; i < 6; i++)
    {
        if (mac[i] != 0x00u) all_zero = RT_FALSE;
        if (mac[i] != 0xFFu) all_ff   = RT_FALSE;
    }

    return (all_zero || all_ff) ? RT_FALSE : RT_TRUE;
}

static int ch395_chip_init(void)
{
    static const uint8_t default_mac[6] = { 0x02, 0x00, 0x00, 0x39, 0x50, 0x01 };
    uint8_t v = 0;
    int i;

    ch395_port_init();
    ch395_hw_reset();

    /* 自检：写 0x65 应读回 0x9A（芯片可能还在上电复位，多试几次） */
    for (i = 0; i < 10; i++)
    {
        v = ch395_check_exist(0x65);
        if (v == 0x9A)
        {
            break;
        }
        rt_thread_mdelay(20);
    }

    if (v != 0x9A)
    {
        rt_kprintf("[ch395] not found (check_exist=0x%02x)\n", v);
        return -RT_EIO;
    }

    ch395_cmd_reset_all();

    v = ch395_init_chip();
    if (v != CH395_ERR_SUCCESS)
    {
        /* 冷启动时芯片可能还没完全就绪，给一次重试 */
        rt_kprintf("[ch395] init failed (0x%02x), retry...\n", v);
        rt_thread_mdelay(200);
        v = ch395_init_chip();
        if (v != CH395_ERR_SUCCESS)
        {
            rt_kprintf("[ch395] init failed (0x%02x)\n", v);
            return -RT_EIO;
        }
    }

    /* 出厂已烧 MAC，INIT 之后读才可靠 */
    ch395_get_mac(s_mac);
    if (!ch395_mac_valid(s_mac))
    {
        /* 读不到可用 MAC：写一个本地管理地址，再 INIT 一次让它生效
         * （手册要求 SET_MAC_ADDR 在 INIT_CH395 之前执行） */
        rt_memcpy(s_mac, default_mac, 6);
        ch395_set_mac(s_mac);

        v = ch395_init_chip();
        if (v != CH395_ERR_SUCCESS)
        {
            rt_kprintf("[ch395] re-init failed (0x%02x)\n", v);
            return -RT_EIO;
        }
    }

    /* MACRAW 只能用在 Socket0 */
    ch395_set_proto_type(CH395_SOCK_MACRAW, CH395_PROTO_MAC_RAW);

    v = ch395_open_socket(CH395_SOCK_MACRAW);
    if (v != CH395_ERR_SUCCESS)
    {
        rt_kprintf("[ch395] open socket0 failed (status=0x%02x)\n", v);
        return -RT_EIO;
    }

    /* MACRAW 必须开 RECV_ALL：
     *   只开 广播|多播|收发使能 时，广播帧（DHCP）能进来，
     *   但发往本机的单播帧（ARP 应答、ICMP 应答）会被芯片过滤掉，
     *   表现就是「DHCP 正常、ping 全超时」。
     * 交给 lwIP 自己按 MAC 过滤，代价只是多一点无关帧。 */
    ch395_set_mac_filt(CH395_MAC_FILT_RECV_ALL | CH395_MAC_FILT_BROADCAST |
                       CH395_MAC_FILT_MULTICAST |
                       CH395_MAC_FILT_RECV_EN | CH395_MAC_FILT_SEND_EN,
                       0, 0);

    return RT_EOK;
}

/* ============================ 对外入口 ============================ */
int ch395_netif_init(void)
{
    if (ch395_chip_init() != RT_EOK)
    {
        return -RT_EIO;
    }

    /* 必须先挂好回调，eth_device_init 里会通过 NIOCTL_GADDR 取 MAC */
    s_dev.parent.init    = ch395_eth_dev_init;
    s_dev.parent.open    = ch395_eth_dev_open;
    s_dev.parent.close   = ch395_eth_dev_close;
    s_dev.parent.control = ch395_eth_dev_control;
    s_dev.eth_rx         = ch395_eth_rx;
    s_dev.eth_tx         = ch395_eth_tx;

    if (eth_device_init(&s_dev, "e0") != RT_EOK)
    {
        rt_kprintf("[ch395] eth_device_init failed\n");
        return -RT_EIO;
    }

    /* 芯片在开 socket / PHY 变化时可能已经把 INT 拉低。先读一次全局中断
     * 把 INT 释放掉，否则引脚一直是低电平，之后再也等不到下降沿，
     * 表现就是「链路正常但一个包都收不到」。 */
    (void)ch395_get_glob_int_all();

    ch395_exti_init();

    /* 释放 INT 与打开 EXTI 之间如果又来了事件，引脚现在已经是低电平，
     * 边沿已经错过，这里按电平补一次通知。 */
    if ((GPIO_IDR(GPIOG_BASE) & (1u << CH395_INT_LINE)) == 0u)
    {
        eth_device_ready(&s_dev);
    }

    s_link_up = RT_FALSE;
    ch395_update_link();
    s_ready = RT_TRUE;

    rt_kprintf("[ch395] e0 up, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
               s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    rt_kprintf("[ch395] driver built %s %s\n", __DATE__, __TIME__);

    return RT_EOK;
}
/* 组件初始化阶段启动：此时 lwIP 的 tcpip 线程已经起来（INIT_PREV_EXPORT） */
INIT_COMPONENT_EXPORT(ch395_netif_init);

/* ============================ 调试命令 ============================ */
/* msh > ch395           芯片状态 + 收发计数
 * msh > ch395 log       最近几帧的帧头（排查 ARP/ICMP 有没有到）
 */
static void ch395_print_ip(uint32_t ip)
{
    rt_kprintf("%u.%u.%u.%u",
               (unsigned)(ip & 0xFFu), (unsigned)((ip >> 8) & 0xFFu),
               (unsigned)((ip >> 16) & 0xFFu), (unsigned)((ip >> 24) & 0xFFu));
}

/* lwIP 自己的视角：netdev 层显示的和 netif 里实际存的可能不一致，
 * ifconfig 走的是 netdev，这里直接看 netif。 */
static void ch395_dump_netif(void)
{
    struct netif *nif;
    char ip[16], gw[16], mask[16];

    nif = netif_find("e0");
    if (nif == RT_NULL)
    {
        rt_kprintf("       netif: not found\n");
        return;
    }

    ip4addr_ntoa_r(&nif->ip_addr, ip, (int)sizeof(ip));
    ip4addr_ntoa_r(&nif->gw, gw, (int)sizeof(gw));
    ip4addr_ntoa_r(&nif->netmask, mask, (int)sizeof(mask));

    rt_kprintf("       netif ip=%s gw=%s mask=%s flags=0x%02x mtu=%u\n",
               ip, gw, mask, (unsigned)nif->flags, (unsigned)nif->mtu);
    rt_kprintf("       netif hwaddr %02x:%02x:%02x:%02x:%02x:%02x\n",
               nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
               nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5]);
}

static void ch395_dump_frames(const char *tag,
                              const struct ch395_frame_hdr *log, uint32_t idx)
{
    int i;
    const char *kind;
    const struct ch395_frame_hdr *e;

    for (i = 0; i < CH395_FRAME_LOG_NUM; i++)
    {
        e = &log[(idx + (uint32_t)i) % CH395_FRAME_LOG_NUM];
        if (e->len == 0u)
        {
            continue;
        }

        if (e->type == 0x0806u)
        {
            kind = (e->proto == 2u) ? "ARP-rep" : "ARP-req";
        }
        else if (e->type == 0x0800u)
        {
            kind = (e->proto == 1u)  ? "ICMP" :
                   (e->proto == 6u)  ? "TCP"  :
                   (e->proto == 17u) ? "UDP"  : "IPv4";
        }
        else
        {
            kind = "?";
        }

        rt_kprintf("%s %-7s %02x:%02x:%02x:%02x:%02x:%02x -> "
                   "%02x:%02x:%02x:%02x:%02x:%02x len %u  ",
                   tag, kind,
                   e->src[0], e->src[1], e->src[2], e->src[3], e->src[4], e->src[5],
                   e->dst[0], e->dst[1], e->dst[2], e->dst[3], e->dst[4], e->dst[5],
                   e->len);
        ch395_print_ip(e->sip);
        rt_kprintf(" -> ");
        ch395_print_ip(e->dip);
        rt_kprintf("\n");
    }
}

static void ch395(int argc, char **argv)
{
    uint8_t  ver, phy, sint;
    uint16_t gint;

    if (!s_ready)
    {
        rt_kprintf("ch395: not initialized\n");
        return;
    }

    ver  = ch395_get_ic_ver();
    phy  = ch395_get_phy_status();
    gint = ch395_get_glob_int_all();
    sint = ch395_get_socket_int(CH395_SOCK_MACRAW);

    rt_kprintf("ch395: ver=0x%02x phy=0x%02x gint=0x%04x sock0=0x%02x\n",
               ver, phy, gint, sint);
    rt_kprintf("       mac=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
               s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5],
               s_link_up ? "up" : "down");
    rt_kprintf("       rx=%u drop=%u tx=%u\n",
               (unsigned)s_rx_frames, (unsigned)s_rx_drop, (unsigned)s_tx_frames);
    rt_kprintf("       fw %s %s\n", __DATE__, __TIME__);

    ch395_dump_netif();

    if (argc >= 2 && rt_strcmp(argv[1], "log") == 0)
    {
        ch395_dump_frames("rx", s_rx_log, s_rx_log_idx);
        ch395_dump_frames("tx", s_tx_log, s_tx_log_idx);
    }

    /* 上面读全局中断会把 INT 释放掉，若还有积压的帧，补一次通知让协议栈去收 */
    if ((gint & CH395_GINT_SOCK0) && (sint & CH395_SINT_RECV))
    {
        eth_device_ready(&s_dev);
    }
}
MSH_CMD_EXPORT(ch395, show CH395 chip status);
