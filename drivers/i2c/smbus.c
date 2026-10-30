/*
 * smbus.c - 严格 SMBus 协议实现 <drivers/i2c/>
 *
 * 全部建立在本目录的 i2c_transfer() 之上，不碰寄存器、不依赖 OS。
 *
 * 协议选择完全由 (cmd, len) 决定，见 smbus.h 的对照表：
 *   cmd = SMBUS_NO_CMD, len 0  —— 只发地址字节          Quick Command
 *   cmd = SMBUS_NO_CMD, len 1  —— [data]               Send / Receive Byte
 *   cmd = 命令码,       len 1  —— [cmd][d0]            Write / Read Byte
 *   cmd = 命令码,       len 2  —— [cmd][lo][hi]        Write / Read Word
 *   cmd = 命令码,       len 3+ —— [cmd][count][data..] Block Write / Read
 *
 * PEC 覆盖范围（规范 §5.4「覆盖消息中的所有字节，包括地址字节和 R/W 位」）：
 *
 *   Quick         : [addr<<1|R/W]                                （规范不给 PEC）
 *   Send Byte     : [addr<<1|0] [data]
 *   Receive Byte  : [addr<<1|1] [data]
 *   Write Byte    : [addr<<1|0] [cmd] [data]                    主机算 PEC 并发送
 *   Read Byte     : [addr<<1|0] [cmd] [addr<<1|1] [data]        从机算 PEC 并发送
 *   Write Word    : [addr<<1|0] [cmd] [lo] [hi]                 主机算
 *   Read Word     : [addr<<1|0] [cmd] [addr<<1|1] [lo] [hi]     从机算
 *   Process Call  : [addr<<1|0] [cmd] [wlo] [whi] [addr<<1|1] [rlo] [rhi]
 *   Block Write   : [addr<<1|0] [cmd] [count] [data...]
 *   Block Read    : [addr<<1|0] [cmd] [addr<<1|1] [count] [data...]
 *
 * 注意重复起始位后的那个地址字节带 R/W=1，它也要参与 CRC —— 这是最常见的写错点。
 * 为了让这个错误不可能发生，下面统一用 pec_for_write()/pec_for_read()，
 * 前者直接吃「写完的整个 tx 缓冲」，后者吃「写段缓冲 + 读段缓冲」，
 * 地址字节由函数自己拼，不靠调用方记得补。
 */
#include "smbus.h"

#define PEC_ON(dev)     (((dev)->flags & SMBUS_F_PEC) != 0)

/* 只给内部 smbus_quick_xfer 用的方向标记 */
#define QUICK_WRITE     0
#define QUICK_READ      1

/* ================= PEC ================= */

uint8_t smbus_pec(uint8_t crc, const uint8_t *buf, unsigned int len)
{
    unsigned int i;

    while (len-- > 0u)
    {
        crc ^= *buf++;
        for (i = 0; i < 8u; i++)
        {
            if (crc & 0x80u)
            {
                crc = (uint8_t)((crc << 1) ^ 0x07u);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

int smbus_selftest(void)
{
    /* CRC-8/SMBUS 的标准校验值：对 ASCII "123456789" 应得 0xF4。
     * 这个值只由多项式/初值/方向决定，与具体实现无关，可以当已知答案测试。 */
    static const uint8_t v[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

    return (smbus_pec(0u, v, sizeof(v)) == 0xF4u) ? 0 : SMBUS_ERR_PEC;
}

/* 写方向 PEC：地址字节(R/W=0) + 整个写缓冲（缓冲里已经是 cmd/count/data 的线上顺序） */
static uint8_t pec_for_write(const struct smbus_dev *dev,
                             const uint8_t *wire, unsigned int len)
{
    uint8_t ab = (uint8_t)(dev->addr << 1);
    uint8_t crc = smbus_pec(0u, &ab, 1);

    if (len != 0u)
    {
        crc = smbus_pec(crc, wire, len);
    }
    return crc;
}

/* 读方向 PEC：地址字节(R/W=0) + 写段 + 地址字节(R/W=1) + 读段
 * 写段传 NULL/0 就是 Receive Byte 那种「没有命令码」的情形 —— 此时总线上
 * 根本没有 addr|W，只有 addr|R，所以不能盲目把 aw 算进去。 */
static uint8_t pec_for_read(const struct smbus_dev *dev,
                            const uint8_t *written, unsigned int wlen,
                            const uint8_t *read, unsigned int rlen)
{
    uint8_t aw = (uint8_t)(dev->addr << 1);
    uint8_t ar = (uint8_t)((dev->addr << 1) | 1u);
    uint8_t crc;

    if (wlen == 0u)
    {
        crc = smbus_pec(0u, &ar, 1);
    }
    else
    {
        crc = smbus_pec(0u, &aw, 1);
        crc = smbus_pec(crc, written, wlen);
        crc = smbus_pec(crc, &ar, 1);
    }

    if (rlen != 0u)
    {
        crc = smbus_pec(crc, read, rlen);
    }
    return crc;
}

/* ================= 内部工具 ================= */

static int smbus_check(const struct smbus_dev *dev)
{
    if (dev == 0 || dev->adap == 0 || dev->addr > 0x7Fu)
    {
        return I2C_ERR_PARAM;
    }
    return 0;
}

static int smbus_send(struct smbus_dev *dev, const uint8_t *buf, uint16_t len)
{
    int ret = i2c_write(dev->adap, dev->addr, buf, len);

    if (ret < 0)
    {
        return ret;
    }
    return (ret == (int)len) ? 0 : I2C_ERR_IO;
}

/* 「先写一段，再重复起始位读」的公共骨架。extra 给读段加标志（如 RECV_LEN）。
 * wbuf 不用 const：i2c_msg.buf 本身是非 const（同一个字段读写共用）。
 *
 * wlen == 0 时总线上没有写段，只发 1 条消息 —— Receive Byte / Quick 就是这种。
 * 这样「有命令码的读」和「没命令码的读」共用同一条路径，区别只是消息条数。 */
static int smbus_read_after(struct smbus_dev *dev,
                            uint8_t *wbuf, uint16_t wlen,
                            uint8_t *rbuf, uint16_t rlen, uint16_t extra_flags)
{
    struct i2c_msg msgs[2];
    struct i2c_msg *rd;
    int num;
    int ret;

    if (wlen == 0u)
    {
        rd  = &msgs[0];
        num = 1;
    }
    else
    {
        msgs[0].addr  = dev->addr;
        msgs[0].flags = 0;
        msgs[0].len   = wlen;
        msgs[0].buf   = wbuf;
        rd  = &msgs[1];
        num = 2;
    }

    rd->addr  = dev->addr;
    rd->flags = (uint16_t)(I2C_M_RD | extra_flags);
    rd->len   = rlen;
    rd->buf   = rbuf;

    ret = i2c_transfer(dev->adap, msgs, num);
    if (ret < 0)
    {
        return ret;
    }
    return (ret == num) ? 0 : I2C_ERR_IO;
}

/* Quick Command：只有地址字节，信息全在 R/W 位里。规范未给它定义 PEC。
 * 读方向和 i2c_core.c 的 i2c_probe_device() 是同一个线上操作（都是 [addr|R] + STOP），
 * 这不是重复实现而是有意的分层重叠：i2c_probe_device 属于 I2C 层（扫描总线用，
 * 不依赖 smbus.c），这里补上 Quick Command 的写方向并归入 SMBus 协议集合。 */
static int smbus_quick_xfer(struct smbus_dev *dev, int read)
{
    struct i2c_msg msg;
    int ret;

    msg.addr  = dev->addr;
    msg.flags = read ? I2C_M_RD : 0;
    msg.len   = 0;
    msg.buf   = 0;

    ret = i2c_transfer(dev->adap, &msg, 1);
    if (ret < 0)
    {
        return ret;
    }
    return (ret == 1) ? 0 : I2C_ERR_IO;
}

/* ================= 统一的读 / 写 =================
 *
 * 两个函数就是全部「写若干字节 / 读若干字节」的协议。每个协议在总线上只是
 * 三件事的组合：写段长 wlen（0 或 1）、读段长 rlen、读段是否走 RECV_LEN。
 *
 *               wlen  rlen        flags
 *   Quick        0     0            0        （走 smbus_quick_xfer，方向由调用决定）
 *   Receive Byte 0     1(+PEC)      0
 *   Read Byte    1     1(+PEC)      0
 *   Read Word    1     2(+PEC)      0
 *   Block Read   1     0或1(+PEC)   RECV_LEN
 */

int smbus_read(struct smbus_dev *dev, int cmd, uint8_t *buf, uint16_t len)
{
    uint8_t tx = 0;
    uint8_t rx[1 + SMBUS_BLOCK_MAX + 1];
    uint16_t wlen = 0;
    uint16_t rlen;
    unsigned int block = 0;
    unsigned int cnt;
    unsigned int off;
    unsigned int i;
    int ret = smbus_check(dev);

    if (ret < 0)
    {
        return ret;
    }

    /* ---- 选定协议：wlen / rlen / block ---- */
    if (cmd == SMBUS_NO_CMD)
    {
        if (len == 0u)
        {
            return smbus_quick_xfer(dev, QUICK_READ);   /* 只发地址，不读数据 */
        }
        if (len != 1u || buf == 0)
        {
            return I2C_ERR_PARAM;                       /* Receive Byte 只有 1 字节 */
        }

        rlen = (uint16_t)(1u + (PEC_ON(dev) ? 1u : 0u));
    }
    else
    {
        if (buf == 0 || len == 0u || len > SMBUS_BLOCK_MAX)
        {
            return I2C_ERR_PARAM;
        }

        tx   = (uint8_t)cmd;
        wlen = 1u;

        if (len <= 2u)
        {
            rlen = (uint16_t)(len + (PEC_ON(dev) ? 1u : 0u));
        }
        else
        {
            block = 1u;
            rlen  = (uint16_t)(PEC_ON(dev) ? 1u : 0u);  /* 尾随字节数，长度等从机给 */
        }
    }

    ret = smbus_read_after(dev, (wlen != 0u) ? &tx : 0, wlen, rx, rlen,
                           (uint16_t)(block ? I2C_M_RECV_LEN : 0));
    if (ret < 0)
    {
        return ret;
    }

    /* ---- 确定数据长度和偏移：块读多一个计数字节 ---- */
    if (block)
    {
        cnt = rx[0];
        if (cnt > SMBUS_BLOCK_MAX || cnt > len)
        {
            return SMBUS_ERR_COUNT;
        }
    }
    else
    {
        cnt = len;
    }

    off = block ? 1u : 0u;

    /* 校验从机附加的 PEC：覆盖 off+cnt 个读段字节（块读时含计数字节） */
    if (PEC_ON(dev) &&
        (pec_for_read(dev, (wlen != 0u) ? &tx : 0, wlen, rx, off + cnt) != rx[off + cnt]))
    {
        return SMBUS_ERR_PEC;
    }

    for (i = 0; i < cnt; i++)
    {
        buf[i] = rx[off + i];
    }
    return (int)cnt;
}

int smbus_write(struct smbus_dev *dev, int cmd, const uint8_t *buf, uint16_t len)
{
    uint8_t tx[1 + 1 + SMBUS_BLOCK_MAX + 1];
    unsigned int n = 0;
    unsigned int i;
    int ret = smbus_check(dev);

    if (ret < 0)
    {
        return ret;
    }

    if (cmd == SMBUS_NO_CMD)
    {
        if (len == 0u)
        {
            return smbus_quick_xfer(dev, QUICK_WRITE);
        }
        if (len != 1u || buf == 0)
        {
            return I2C_ERR_PARAM;                       /* Send Byte 只有 1 字节 */
        }

        /* 没有命令码：不写 tx[0]，下面直接就是 [data][PEC?] */
    }
    else
    {
        if (buf == 0 || len == 0u || len > SMBUS_BLOCK_MAX)
        {
            return I2C_ERR_PARAM;
        }

        tx[n++] = (uint8_t)cmd;

        if (len >= 3u)
        {
            tx[n++] = (uint8_t)len;                     /* Block Write 的计数字节 */
        }
    }

    for (i = 0; i < len; i++)
    {
        tx[n++] = buf[i];
    }

    if (PEC_ON(dev))
    {
        tx[n] = pec_for_write(dev, tx, n);              /* 覆盖实际发出的全部字节 */
        n++;
    }

    return smbus_send(dev, tx, (uint16_t)n);
}

/* ================= Process Call ================= */

int smbus_process_call(struct smbus_dev *dev, uint8_t cmd,
                       uint16_t wdata, uint16_t *rdata)
{
    uint8_t tx[3];
    uint8_t rx[3];
    unsigned int n = PEC_ON(dev) ? 3u : 2u;
    int ret = smbus_check(dev);

    if (ret < 0 || rdata == 0)
    {
        return I2C_ERR_PARAM;
    }

    /* SMBus 是小端：低字节在前 */
    tx[0] = cmd;
    tx[1] = (uint8_t)(wdata & 0xFFu);
    tx[2] = (uint8_t)(wdata >> 8);

    ret = smbus_read_after(dev, tx, 3, rx, (uint16_t)n, 0);
    if (ret < 0)
    {
        return ret;
    }

    if (PEC_ON(dev) && (pec_for_read(dev, tx, 3, rx, 2) != rx[2]))
    {
        return SMBUS_ERR_PEC;
    }

    *rdata = (uint16_t)(rx[0] | ((uint16_t)rx[1] << 8));
    return 0;
}

/* ================= Block Write-Block Read Process Call ================= */

int smbus_block_process_call(struct smbus_dev *dev, uint8_t cmd,
                             const uint8_t *wdata, uint8_t wcount,
                             uint8_t *rdata, uint8_t *rcount)
{
    uint8_t tx[2 + SMBUS_BLOCK_MAX];
    uint8_t rx[1 + SMBUS_BLOCK_MAX + 1];
    unsigned int n = PEC_ON(dev) ? 1u : 0u;
    unsigned int i;
    unsigned int cnt;
    unsigned int wlen;
    int ret = smbus_check(dev);

    if (ret < 0 || wdata == 0 || rdata == 0 || rcount == 0)
    {
        return I2C_ERR_PARAM;
    }
    if (wcount == 0u || wcount > SMBUS_BLOCK_MAX)
    {
        return SMBUS_ERR_COUNT;
    }

    tx[0] = cmd;
    tx[1] = wcount;
    for (i = 0; i < wcount; i++)
    {
        tx[2u + i] = wdata[i];
    }
    wlen = 2u + wcount;

    ret = smbus_read_after(dev, tx, (uint16_t)wlen, rx, (uint16_t)n, I2C_M_RECV_LEN);
    if (ret < 0)
    {
        return ret;
    }

    cnt = rx[0];
    if (cnt > SMBUS_BLOCK_MAX)
    {
        return SMBUS_ERR_COUNT;
    }

    if (PEC_ON(dev) && (pec_for_read(dev, tx, wlen, rx, cnt + 1u) != rx[1u + cnt]))
    {
        return SMBUS_ERR_PEC;
    }

    for (i = 0; i < cnt; i++)
    {
        rdata[i] = rx[1u + i];
    }
    *rcount = (uint8_t)cnt;

    return 0;
}
