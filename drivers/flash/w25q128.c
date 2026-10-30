/*
 * w25q128.c - W25Q128 / NM25Q128 SPI NOR FLASH 驱动实现 <drivers/flash/>
 *
 * 全部建立在 spi.h 之上，不碰寄存器、不依赖 OS。
 *
 * ── 每次操作都是一个「CS 时隙」────────────────────────────────────────
 *   25Q 的时序特点：一条命令从 CS 拉低开始、到 CS 拉高才算结束，中间的命令码、
 *   3 字节地址、数据必须在同一个 CS 窗口里连续发出。所以本文件几乎每条语句都是
 *   一次 spi_sync / spi_write_then_read，而不是「先发命令再发数据」两次调用 ——
 *   分成两次会在中间释放 CS，片子会把它当成两条独立命令。
 *
 *   读：      [03][A23..A0] + 连续读 N 字节
 *   页编程：  [02][A23..A0] + 连续写 N 字节（N ≤ 256 且不跨页）
 *   擦扇区：  [20][A23..A0]
 *   读状态：  [05] + 1 字节
 *   JEDEC：   [9F] + 3 字节
 */
#include <string.h>

#include "w25q128.h"
#include "w25q128_config.h"

/* ============================ 命令码 ============================ */
#define W25Q_CMD_WRITE_ENABLE   0x06u
#define W25Q_CMD_READ_SR1       0x05u
#define W25Q_CMD_READ_DATA      0x03u
#define W25Q_CMD_PAGE_PROGRAM   0x02u
#define W25Q_CMD_SECTOR_ERASE   0x20u
#define W25Q_CMD_CHIP_ERASE     0xC7u
#define W25Q_CMD_JEDEC_ID       0x9Fu

/* 单次 SPI 事务最多搬多少字节：spi.h 的 len 是 uint16_t，这里再留小一点，
 * 让 CS 有效期内不会太久（也方便上层在中间插入更高优先级的传输） */
#define W25Q_XFER_CHUNK         1024u

static struct w25q128_dev *s_default;

/* ============================ 小工具 ============================ */

/* 只发一个命令字节（写使能、整片擦除这种不带参数的命令） */
static int w25q_cmd_only(struct w25q128_dev *dev, uint8_t cmd)
{
    return spi_write(&dev->spi, &cmd, 1);
}

/* 发「命令 + 24 位地址」，同一 CS 时隙内 */
static int w25q_cmd_addr(struct w25q128_dev *dev, uint8_t cmd, uint32_t addr)
{
    uint8_t buf[4];

    buf[0] = cmd;
    buf[1] = (uint8_t)((addr >> 16) & 0xFFu);
    buf[2] = (uint8_t)((addr >> 8) & 0xFFu);
    buf[3] = (uint8_t)(addr & 0xFFu);

    return spi_write(&dev->spi, buf, 4);
}

/* 读状态寄存器 1。成功 0，失败返回负数（不要用 0xFF 表示错误，
 * 那会让 wait_ready 把「总线坏了」误判成「一直忙」）。 */
static int w25q_read_sr1(struct w25q128_dev *dev, uint8_t *sr)
{
    uint8_t cmd = W25Q_CMD_READ_SR1;
    int ret;

    ret = spi_write_then_read(&dev->spi, &cmd, 1, sr, 1);
    return (ret < 0) ? ret : 0;
}

/* 轮询 BUSY 直到落下。poll_max 是轮询次数上限（时间随 SCLK 变化）。 */
static int w25q_wait_busy(struct w25q128_dev *dev, uint32_t poll_max)
{
    uint32_t i;

    for (i = 0; i < poll_max; i++)
    {
        uint8_t sr;
        int ret = w25q_read_sr1(dev, &sr);

        if (ret < 0)
        {
            return ret;                 /* 总线错误直接上抛 */
        }
        if ((sr & W25Q_SR1_BUSY) == 0u)
        {
            return 0;
        }
    }
    return W25Q_ERR_TIMEOUT;
}

static int w25q_range_ok(const struct w25q128_dev *dev, uint32_t addr, uint32_t len)
{
    if (addr > dev->capacity)
    {
        return 0;
    }
    return (len <= (dev->capacity - addr)) ? 1 : 0;
}

/* ============================ 初始化 ============================ */

int w25q128_init(struct w25q128_dev *dev, struct spi_controller *ctlr,
                 uint32_t cs_pin)
{
    if (dev == NULL || ctlr == NULL)
    {
        return SPI_ERR_PARAM;
    }

    memset(dev, 0, sizeof(*dev));
    dev->spi.ctlr = ctlr;
    dev->spi.max_speed_hz = W25Q128_SPEED_HZ;
    dev->spi.mode = W25Q128_SPI_MODE;
    dev->spi.bits_per_word = 8u;
    dev->spi.cs_pin = cs_pin;

    /* 刻意不读 ID：init 跑在调度器起来之前，不能碰总线（见头文件说明）。
     * capacity == 0 就是「还没探测」的标记，第一次操作时自动补。 */
    dev->capacity = 0u;
    dev->jedec = 0u;
    return 0;
}

int w25q128_probe(struct w25q128_dev *dev)
{
    uint8_t cmd = W25Q_CMD_JEDEC_ID;
    uint8_t id[3];
    int ret;

    if (dev == NULL || dev->spi.ctlr == NULL)
    {
        return SPI_ERR_PARAM;
    }

    /* 上电时可能还在上一次擦除里，先等它忙完。这里用短上限（而非整片擦除
     * 那个几十秒的上限）：片子不在/坏了时该尽快失败，而不是卡住几十秒。
     * 真正长时间的整片擦除由 w25q128_erase_chip() 自己等。 */
    ret = w25q_wait_busy(dev, W25Q128_BUSY_POLL_MAX);
    if (ret < 0 && ret != W25Q_ERR_TIMEOUT)
    {
        return ret;
    }

    ret = spi_write_then_read(&dev->spi, &cmd, 1, id, 3);
    if (ret < 0)
    {
        return ret;
    }

    dev->jedec = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];

    /* 第三个字节是容量码：0x10 = 64KB … 0x18 = 16MB。
     * 0x19 起是 256Mbit 以上，读写地址要 32 位（4 字节地址模式），
     * 本驱动没实现，明确拒绝而不是拿 24 位地址去写坏数据。 */
    if ((id[2] < 0x10u) || (id[2] > 0x18u))
    {
        dev->capacity = 0u;
        return W25Q_ERR_ID;
    }

    dev->capacity = 1u << id[2];
    return 0;
}

/* 第一次真正用总线时补一次探测 */
static int w25q_ensure(struct w25q128_dev *dev)
{
    if (dev->capacity != 0u)
    {
        return 0;
    }
    return w25q128_probe(dev);
}

/* ============================ 读 ============================ */

int w25q128_read(struct w25q128_dev *dev, uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;
    int ret;

    if (dev == NULL || (buf == NULL && len > 0u))
    {
        return SPI_ERR_PARAM;
    }
    ret = w25q_ensure(dev);
    if (ret < 0)
    {
        return ret;
    }
    if (w25q_range_ok(dev, addr, len) == 0)
    {
        return W25Q_ERR_RANGE;
    }

    while (len > 0u)
    {
        uint8_t hdr[4];
        uint16_t chunk = (len > W25Q_XFER_CHUNK) ? (uint16_t)W25Q_XFER_CHUNK : (uint16_t)len;

        hdr[0] = W25Q_CMD_READ_DATA;
        hdr[1] = (uint8_t)((addr >> 16) & 0xFFu);
        hdr[2] = (uint8_t)((addr >> 8) & 0xFFu);
        hdr[3] = (uint8_t)(addr & 0xFFu);

        ret = spi_write_then_read(&dev->spi, hdr, 4, p, chunk);
        if (ret < 0)
        {
            return ret;
        }

        addr += chunk;
        p += chunk;
        len -= chunk;
    }

    return 0;
}

/* ============================ 写（要求已擦除）============================ */

int w25q128_write(struct w25q128_dev *dev, uint32_t addr,
                  const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int ret;

    if (dev == NULL || (buf == NULL && len > 0u))
    {
        return SPI_ERR_PARAM;
    }
    ret = w25q_ensure(dev);
    if (ret < 0)
    {
        return ret;
    }
    if (w25q_range_ok(dev, addr, len) == 0)
    {
        return W25Q_ERR_RANGE;
    }

    while (len > 0u)
    {
        /* 本页还能写多少：25Q 的页编程跨页会回卷到页首（同 AT24C02），
         * 必须在这里切开，调用方不用关心起始地址是否对齐。 */
        uint32_t room = W25Q_PAGE_SIZE - (addr % W25Q_PAGE_SIZE);
        uint32_t chunk = (len < room) ? len : room;

        struct spi_message msg;
        struct spi_transfer x[2];
        uint8_t hdr[4];

        ret = w25q_cmd_only(dev, W25Q_CMD_WRITE_ENABLE);
        if (ret < 0)
        {
            return ret;
        }

        hdr[0] = W25Q_CMD_PAGE_PROGRAM;
        hdr[1] = (uint8_t)((addr >> 16) & 0xFFu);
        hdr[2] = (uint8_t)((addr >> 8) & 0xFFu);
        hdr[3] = (uint8_t)(addr & 0xFFu);

        /* 命令 + 地址 + 数据必须在同一个 CS 时隙里连续发出 */
        spi_message_init(&msg);
        memset(&x[0], 0, sizeof(x[0]));
        x[0].tx_buf = hdr;
        x[0].len = 4u;
        memset(&x[1], 0, sizeof(x[1]));
        x[1].tx_buf = p;
        x[1].len = (uint16_t)chunk;

        if (spi_message_add_tail(&x[0], &msg) != 0 ||
            spi_message_add_tail(&x[1], &msg) != 0)
        {
            return SPI_ERR_NOMEM;
        }

        ret = spi_sync(&dev->spi, &msg);
        if (ret < 0)
        {
            return ret;
        }

        /* 页编程典型 0.7ms / 最大 3ms，写完必须等它落盘才能发下一条命令 */
        ret = w25q_wait_busy(dev, W25Q128_BUSY_POLL_MAX);
        if (ret < 0)
        {
            return ret;
        }

        addr += chunk;
        p += chunk;
        len -= chunk;
    }

    return 0;
}

/* ============================ 读-改-写 ============================ */

int w25q128_write_erase(struct w25q128_dev *dev, uint32_t addr,
                        const void *buf, uint32_t len, void *scratch)
{
    uint8_t *sec = (uint8_t *)scratch;
    const uint8_t *src = (const uint8_t *)buf;
    int ret;

    if (dev == NULL || scratch == NULL || (buf == NULL && len > 0u))
    {
        return SPI_ERR_PARAM;
    }
    ret = w25q_ensure(dev);
    if (ret < 0)
    {
        return ret;
    }
    if (w25q_range_ok(dev, addr, len) == 0)
    {
        return W25Q_ERR_RANGE;
    }

    while (len > 0u)
    {
        uint32_t sec_start = addr & ~(W25Q_SECTOR_SIZE - 1u);
        uint32_t off = addr - sec_start;
        uint32_t n = W25Q_SECTOR_SIZE - off;

        if (n > len)
        {
            n = len;
        }

        /* 1) 整个扇区读出来 */
        ret = w25q128_read(dev, sec_start, sec, W25Q_SECTOR_SIZE);
        if (ret < 0)
        {
            return ret;
        }

        /* 2) 内容没变就别擦 —— 擦除是最伤寿命的操作 */
        if (memcmp(sec + off, src, n) != 0)
        {
            memcpy(sec + off, src, n);

            ret = w25q128_erase_sector(dev, sec_start);
            if (ret < 0)
            {
                return ret;
            }

            /* 3) 整块写回（内部按 256 字节页切分） */
            ret = w25q128_write(dev, sec_start, sec, W25Q_SECTOR_SIZE);
            if (ret < 0)
            {
                return ret;
            }
        }

        addr += n;
        src += n;
        len -= n;
    }

    return 0;
}

/* ============================ 擦除 ============================ */

int w25q128_erase_sector(struct w25q128_dev *dev, uint32_t addr)
{
    int ret;

    if (dev == NULL)
    {
        return SPI_ERR_PARAM;
    }
    ret = w25q_ensure(dev);
    if (ret < 0)
    {
        return ret;
    }
    if ((addr % W25Q_SECTOR_SIZE) != 0u)
    {
        return W25Q_ERR_ALIGN;      /* 不替调用方取整：擦错扇区不可恢复 */
    }
    if (addr >= dev->capacity)
    {
        return W25Q_ERR_RANGE;
    }

    ret = w25q_cmd_only(dev, W25Q_CMD_WRITE_ENABLE);
    if (ret < 0)
    {
        return ret;
    }

    ret = w25q_cmd_addr(dev, W25Q_CMD_SECTOR_ERASE, addr);
    if (ret < 0)
    {
        return ret;
    }

    return w25q_wait_busy(dev, W25Q128_BUSY_POLL_MAX);
}

int w25q128_erase_chip(struct w25q128_dev *dev)
{
    int ret;

    if (dev == NULL)
    {
        return SPI_ERR_PARAM;
    }
    ret = w25q_ensure(dev);
    if (ret < 0)
    {
        return ret;
    }

    ret = w25q_cmd_only(dev, W25Q_CMD_WRITE_ENABLE);
    if (ret < 0)
    {
        return ret;
    }

    ret = w25q_cmd_only(dev, W25Q_CMD_CHIP_ERASE);
    if (ret < 0)
    {
        return ret;
    }

    return w25q_wait_busy(dev, W25Q128_CHIP_ERASE_POLL_MAX);
}

/* ============================ 状态 ============================ */

uint8_t w25q128_read_status(struct w25q128_dev *dev)
{
    uint8_t sr = 0xFFu;

    if (dev != NULL && w25q_ensure(dev) == 0)
    {
        (void)w25q_read_sr1(dev, &sr);
    }
    return sr;
}

int w25q128_wait_ready(struct w25q128_dev *dev)
{
    int ret;

    if (dev == NULL)
    {
        return SPI_ERR_PARAM;
    }
    ret = w25q_ensure(dev);
    if (ret < 0)
    {
        return ret;
    }
    return w25q_wait_busy(dev, W25Q128_BUSY_POLL_MAX);
}

/* ============================ 默认实例 ============================ */

void w25q128_set_default(struct w25q128_dev *dev)
{
    s_default = dev;
}

struct w25q128_dev *w25q128_get_default(void)
{
    return s_default;
}
