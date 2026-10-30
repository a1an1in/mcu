/*
 * i2c_bit.c - 通用 I2C bit-bang 时序状态机 <drivers/i2c/>
 *
 * 这一层只认「引脚原语 + 一个 µs 延时」（struct i2c_bit_ops），
 * 不知道引脚名、不知道主频、不知道用的是哪个 RTOS。
 * 换 SoC 时另写一份「填 ops」的文件即可，时序代码不用动。
 *
 * 时序规则：
 *   START  : SCL 高电平期间 SDA 由高变低
 *   STOP   : SCL 高电平期间 SDA 由低变高
 *   数据   : SCL 低电平期间改变 SDA，SCL 高电平期间采样
 *   第 9 个时钟：发送方释放 SDA，接收方拉低表示 ACK
 *
 * 时钟延展（clock stretching）：
 *   每次释放 SCL 之后都回读引脚，若从机仍把它拉低，就等它放高，
 *   最长等 ops->stretch_timeout_us。这也是 SMBus 的 tTIMEOUT 检查。
 *   注意：要真正支持延展，SCL 必须配成开漏输出（见 i2c_config.h），
 *   推挽输出时这一项退化为「确认 SCL 确实被拉高」（能发现总线对地短路）。
 */
#include "i2c.h"

#define BIT_OPS(adap)   ((struct i2c_bit_ops *)((adap)->algo_data))

/* 等 ACK 时最多轮询多少次（对齐原厂 waittime > 250 的兜底） */
#define I2C_ACK_POLL        250u

/* 等时钟延展时的轮询粒度（µs） */
#define I2C_STRETCH_STEP    10u

static void bit_delay(struct i2c_adapter *adap)
{
    struct i2c_bit_ops *ops = BIT_OPS(adap);

    if (ops->delay_us != 0)
    {
        ops->delay_us(ops->ctx, ops->half_period_us);
    }
}

static void bit_sda(struct i2c_adapter *adap, int level)
{
    struct i2c_bit_ops *ops = BIT_OPS(adap);

    ops->set_sda(ops->ctx, level);
}

static void bit_scl(struct i2c_adapter *adap, int level)
{
    struct i2c_bit_ops *ops = BIT_OPS(adap);

    ops->set_scl(ops->ctx, level);
}

static int bit_sda_get(struct i2c_adapter *adap)
{
    struct i2c_bit_ops *ops = BIT_OPS(adap);

    return ops->get_sda(ops->ctx) ? 1 : 0;
}

/* 释放 SCL 并等它真的变高。
 * 返回值：0 = SCL 已经是高；I2C_ERR_TIMEOUT = 从机拉低太久（超过 tTIMEOUT）。 */
static int bit_release_scl(struct i2c_adapter *adap)
{
    struct i2c_bit_ops *ops = BIT_OPS(adap);
    unsigned int waited = 0;

    ops->set_scl(ops->ctx, 1);

    if (ops->get_scl == 0 || ops->delay_us == 0)
    {
        return 0;                       /* 无法回读：不做延展检测 */
    }

    while (ops->get_scl(ops->ctx) == 0)
    {
        if (waited >= ops->stretch_timeout_us)
        {
            return I2C_ERR_TIMEOUT;
        }
        ops->delay_us(ops->ctx, I2C_STRETCH_STEP);
        waited += I2C_STRETCH_STEP;
    }

    return 0;
}

/* 起始位，同时兼作「重复起始位」：
 * 上一次传输结束后 SCL 为低，先释放 SDA，再把 SCL 拉高，
 * 然后在 SCL 高电平期间把 SDA 拉低 —— 这就是一次合法的 (重复) START。 */
static int bit_start(struct i2c_adapter *adap)
{
    int ret;

    bit_sda(adap, 1);                   /* 释放 SDA，允许它被上拉拉高 */
    bit_delay(adap);

    ret = bit_release_scl(adap);
    if (ret < 0)
    {
        return ret;
    }
    bit_delay(adap);

    bit_sda(adap, 0);                   /* SCL 高时 SDA 下降沿 = START */
    bit_delay(adap);
    bit_scl(adap, 0);                   /* 钳住总线，准备发第一位 */
    bit_delay(adap);

    return 0;
}

/* 停止位：SCL 低时先把 SDA 拉低，再抬 SCL，最后在 SCL 高电平期间释放 SDA。
 * STOP 是收尾动作，即使延展超时也不再有可做的补救，忽略返回值。 */
static void bit_stop(struct i2c_adapter *adap)
{
    bit_sda(adap, 0);
    bit_delay(adap);
    (void)bit_release_scl(adap);
    bit_delay(adap);
    bit_sda(adap, 1);                   /* SCL 高时 SDA 上升沿 = STOP */
    bit_delay(adap);
}

/* 发一个字节（高位先发），返回 0 = 收到 ACK，I2C_ERR_NACK = 无应答 */
static int bit_write_byte(struct i2c_adapter *adap, uint8_t val)
{
    unsigned int i;
    unsigned int poll;
    int ret;

    for (i = 0; i < 8u; i++)
    {
        bit_sda(adap, (val & 0x80u) ? 1 : 0);
        bit_delay(adap);

        ret = bit_release_scl(adap);
        if (ret < 0)
        {
            return ret;
        }
        bit_delay(adap);

        bit_scl(adap, 0);
        bit_delay(adap);
        val = (uint8_t)(val << 1);
    }

    /* 第 9 个时钟：释放 SDA，把总线让给从机回 ACK */
    bit_sda(adap, 1);
    bit_delay(adap);

    ret = bit_release_scl(adap);
    if (ret < 0)
    {
        return ret;
    }
    bit_delay(adap);

    for (poll = 0; poll < I2C_ACK_POLL; poll++)
    {
        if (bit_sda_get(adap) == 0)
        {
            break;                      /* 从机拉低 = ACK */
        }
    }

    bit_scl(adap, 0);
    bit_delay(adap);

    return (poll < I2C_ACK_POLL) ? 0 : I2C_ERR_NACK;
}

/* 读一个字节；ack = 1 回 ACK（后面还要继续读），ack = 0 回 NACK（最后一个字节）。
 * 返回值：0 = 成功（*out 有效），负值 = 出错。 */
static int bit_read_byte(struct i2c_adapter *adap, uint8_t *out, int ack)
{
    unsigned int i;
    uint8_t val = 0;
    int ret;

    bit_sda(adap, 1);                   /* 释放 SDA，交给从机驱动 */

    for (i = 0; i < 8u; i++)
    {
        bit_delay(adap);

        ret = bit_release_scl(adap);
        if (ret < 0)
        {
            return ret;
        }
        bit_delay(adap);

        val = (uint8_t)((val << 1) | (bit_sda_get(adap) ? 1u : 0u));
        bit_scl(adap, 0);
    }

    /* 第 9 个时钟：主机回 ACK / NACK */
    bit_sda(adap, ack ? 0 : 1);
    bit_delay(adap);

    ret = bit_release_scl(adap);
    if (ret < 0)
    {
        return ret;
    }
    bit_delay(adap);

    bit_scl(adap, 0);
    bit_delay(adap);
    bit_sda(adap, 1);                   /* 释放，为下一个字节/停止位做准备 */

    *out = val;
    return 0;
}

/* ================= 一次完整传输 =================
 * 逐条处理 msg：每条前面发 (重复) START + 地址字节，末尾统一 STOP。
 * 中途 NACK / 出错也先补一个 STOP，把总线放回空闲态，否则后续传输全废。
 */

/* 读一个 msg。普通读直接按 msg->len 读；带 I2C_M_RECV_LEN 时按从机给的计数动态读
 * （SMBus Block Read 必须这样，否则无法在正确的字节上回 NACK）。 */
static int bit_read_msg(struct i2c_adapter *adap, struct i2c_msg *msg)
{
    unsigned int total;
    unsigned int idx = 0;
    unsigned int n;
    int ret;

    if (msg->flags & I2C_M_RECV_LEN)
    {
        /* 第 1 个字节是计数 n：回 ACK，因为后面还有数据 */
        ret = bit_read_byte(adap, &msg->buf[0], 1);
        if (ret < 0)
        {
            return ret;
        }

        n = msg->buf[0];
        if (n > I2C_RECV_LEN_MAX)
        {
            return I2C_ERR_PARAM;       /* 从机返回的长度超出规范 */
        }

        total = 1u + n + msg->len;      /* 计数 + n 个数据 + 尾随（0 或 1=PEC）*/
        idx = 1;
    }
    else
    {
        total = msg->len;
    }

    while (idx < total)
    {
        /* 只有整段最后一个字节回 NACK */
        ret = bit_read_byte(adap, &msg->buf[idx], (idx + 1u) < total);
        if (ret < 0)
        {
            return ret;
        }
        idx++;
    }

    return 0;
}

static int i2c_bit_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
    int i;
    int j;
    int ret;

    if (adap == 0 || msgs == 0 || num <= 0)
    {
        return I2C_ERR_PARAM;
    }
    if (BIT_OPS(adap) == 0 || BIT_OPS(adap)->set_sda == 0)
    {
        return I2C_ERR_PARAM;
    }

    for (i = 0; i < num; i++)
    {
        struct i2c_msg *msg = &msgs[i];

        if (msg->flags & I2C_M_TEN)
        {
            return I2C_ERR_PARAM;       /* 10 位地址未实现 */
        }

        /* I2C_M_NOSTART = 「接着上一段继续发」：既不发 START，也**不发地址字节**
         * （Linux 语义）。i2c_reg_write() 正是靠它把 [reg][data...] 拼成一次连续写；
         * 如果这里仍发地址，那个地址字节会被从机当成第一个数据字节收下 ——
         * 整段数据错位一格、末尾丢一个字节（EEPROM 表现为每次写都写歪）。 */
        if ((i == 0) || ((msg->flags & I2C_M_NOSTART) == 0))
        {
            uint8_t addr_byte;

            ret = bit_start(adap);
            if (ret < 0)
            {
                bit_stop(adap);
                return ret;
            }

            addr_byte = (uint8_t)((msg->addr << 1) | ((msg->flags & I2C_M_RD) ? 1u : 0u));
            ret = bit_write_byte(adap, addr_byte);
            if (ret < 0)
            {
                bit_stop(adap);
                return ret;
            }
        }

        if (msg->flags & I2C_M_RD)
        {
            ret = bit_read_msg(adap, msg);
            if (ret < 0)
            {
                bit_stop(adap);
                return ret;
            }
        }
        else
        {
            for (j = 0; j < (int)msg->len; j++)
            {
                ret = bit_write_byte(adap, msg->buf[j]);
                if (ret < 0)
                {
                    bit_stop(adap);
                    return ret;
                }
            }
        }
    }

    bit_stop(adap);
    return num;
}

/* 总线恢复：上电/异常时从机可能把 SDA 拉死，导致发不出 START。
 * 打 9 个 SCL 脉冲让从机把剩下的位吐完，再补一个 STOP。 */
int i2c_bit_recover(struct i2c_adapter *adap)
{
    unsigned int i;
    int ret = 0;

    if (adap == 0 || BIT_OPS(adap) == 0 || BIT_OPS(adap)->set_sda == 0)
    {
        return I2C_ERR_PARAM;
    }

    bit_sda(adap, 1);
    bit_delay(adap);

    for (i = 0; i < 9u; i++)
    {
        (void)bit_release_scl(adap);
        bit_delay(adap);
        bit_scl(adap, 0);
        bit_delay(adap);
    }

    bit_stop(adap);

    /* 空闲态两条线都应为高。任一为低说明没有上拉、或者总线被拉死。 */
    if (bit_sda_get(adap) == 0)
    {
        ret = I2C_ERR_BUS;
    }
    if (BIT_OPS(adap)->get_scl != 0 && BIT_OPS(adap)->get_scl(BIT_OPS(adap)->ctx) == 0)
    {
        ret = I2C_ERR_BUS;
    }

    return ret;
}

static uint32_t i2c_bit_functionality(struct i2c_adapter *adap)
{
    (void)adap;
    return 3u;                          /* I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL */
}

const struct i2c_algorithm i2c_bit_algorithm =
{
    .master_xfer   = i2c_bit_xfer,
    .functionality = i2c_bit_functionality,
};
