/*
 * spi_core.c - SPI 通用核心 <drivers/spi/>
 *
 * 本文件**不认识任何芯片**：没有寄存器、没有时钟、没有 CS 引脚怎么拉，
 * 只有四件事：
 *
 *   1) 控制器注册表            —— 板级把 spi0/spi1... 挂进来；
 *   2) 参数校验                —— 设备 / 消息 / 段的一致性；
 *   3) 一个 CS 时隙的执行编排  —— 拉 CS → 逐段搬 → 段间延时 / cs_change → 放 CS；
 *   4) 总线互斥量              —— 用 OSAL 的 osal_mutex_*，本文件不含 RTOS 头。
 *
 * 真正的字节搬运在 SoC 层（stm32f1_spi.c 的 transfer_one）。
 *
 * 为什么不在这里做重试：SPI 没有 ACK，没有「从机忙」这种可恢复失败 ——
 * 出错就是时钟配置不对 / 线没接好 / 器件的 CS 没选中，重试只会把同样的
 * 错误再演一遍。i2c 有 retries 是因为那里 NACK 是常态。
 */
#include <stddef.h>

#include <osal.h>

#include "spi.h"

/* 每条总线一把互斥量：一次 spi_sync 必须独占 SCLK/MOSI 到底，
 * 否则两个线程各自拉自己的 CS、字节会串到别人的器件上。
 * 锁用 OSAL 的互斥量，不用 rt_mutex —— 这样本文件不需要任何 OS 头，
 * 换 Zephyr/FreeRTOS 只换 osal 后端。 */
#define SPI_LOCK_TIMEOUT_MS     1000u

static struct spi_controller *s_ctlr[SPI_MAX_CONTROLLERS];
static osal_mutex_t s_lock[SPI_MAX_CONTROLLERS];

/* ============================ 总线锁 ============================ */

int spi_bus_lock(struct spi_controller *ctlr)
{
    int nr = spi_controller_nr(ctlr);

    if ((nr < 0) || (s_lock[nr] == OSAL_NULL))
    {
        return SPI_ERR_PARAM;
    }
    if (osal_mutex_lock(s_lock[nr], SPI_LOCK_TIMEOUT_MS) != OSAL_OK)
    {
        return SPI_ERR_BUSY;
    }
    return 0;
}

void spi_bus_unlock(struct spi_controller *ctlr)
{
    int nr = spi_controller_nr(ctlr);

    if ((nr < 0) || (s_lock[nr] == OSAL_NULL))
    {
        return;
    }
    osal_mutex_unlock(s_lock[nr]);
}

/* ============================ 控制器注册 ============================ */

int spi_add_controller(struct spi_controller *ctlr)
{
    int i;

    if (ctlr == NULL || ctlr->algo == NULL || ctlr->algo->transfer_one == NULL)
    {
        return SPI_ERR_PARAM;
    }

    /* 重复注册同一个对象：直接返回原编号，便于板级初始化函数被调用两次 */
    for (i = 0; i < SPI_MAX_CONTROLLERS; i++)
    {
        if (s_ctlr[i] == ctlr)
        {
            return i;
        }
    }

    for (i = 0; i < SPI_MAX_CONTROLLERS; i++)
    {
        if (s_ctlr[i] == NULL)
        {
            /* 在注册时就把锁建好：这个函数由板级在单线程阶段调用，
             * 不会出现「两个线程同时首次加锁、各建一把锁」的竞态。
             * 建互斥量只分配对象，不依赖当前线程，调度器起来之前也安全。 */
            if (s_lock[i] == OSAL_NULL)
            {
                if (osal_mutex_create(&s_lock[i]) != OSAL_OK)
                {
                    return SPI_ERR_NOMEM;
                }
            }
            s_ctlr[i] = ctlr;
            return i;
        }
    }

    return SPI_ERR_NOMEM;   /* 表满了 */
}

struct spi_controller *spi_get_controller(unsigned int nr)
{
    if (nr >= SPI_MAX_CONTROLLERS)
    {
        return NULL;
    }
    return s_ctlr[nr];
}

int spi_controller_nr(struct spi_controller *ctlr)
{
    int i;

    if (ctlr == NULL)
    {
        return SPI_ERR_PARAM;
    }
    for (i = 0; i < SPI_MAX_CONTROLLERS; i++)
    {
        if (s_ctlr[i] == ctlr)
        {
            return i;
        }
    }
    return SPI_ERR_PARAM;
}

/* ============================ 片选 ============================ */

static void spi_cs(struct spi_controller *ctlr, struct spi_device *spi, int active)
{
    if (spi->cs_pin == SPI_CS_NONE)
    {
        return;             /* CS 不归软件管 */
    }
    if (ctlr->algo->set_cs != NULL)
    {
        ctlr->algo->set_cs(ctlr, spi, active);
    }
}

/* ============================ 同步传输 ============================ */

int spi_sync(struct spi_device *spi, struct spi_message *msg)
{
    struct spi_controller *ctlr;
    unsigned int i;
    int ret = 0;

    if (spi == NULL || msg == NULL || spi->ctlr == NULL)
    {
        return SPI_ERR_PARAM;
    }

    ctlr = spi->ctlr;
    if (ctlr->algo == NULL || ctlr->algo->transfer_one == NULL)
    {
        return SPI_ERR_PARAM;
    }
    if (msg->n == 0u)
    {
        return SPI_ERR_PARAM;
    }

    msg->spi = spi;
    msg->actual_length = 0u;
    msg->status = 0;

    if (spi_bus_lock(ctlr) != 0)
    {
        msg->status = SPI_ERR_BUSY;
        return msg->status;
    }

    /* 每次消息都重新应用一次参数：不同器件挂在同一条总线上时
     * （模式 / 速率各不同）这样最省事，也不用维护「当前配置」缓存。 */
    if (ctlr->algo->setup != NULL)
    {
        ret = ctlr->algo->setup(ctlr, spi);
        if (ret < 0)
        {
            goto out;
        }
    }

    spi_cs(ctlr, spi, 1);

    for (i = 0; i < msg->n; i++)
    {
        struct spi_transfer *xfer = &msg->xfer[i];

        if (xfer->len > 0u)
        {
            ret = ctlr->algo->transfer_one(ctlr, spi, xfer);
            if (ret < 0)
            {
                break;
            }
            msg->actual_length += xfer->len;
        }

        /* 段间延时：有的器件要求「命令之后等 10µs 才能读结果」 */
        if ((xfer->delay_usecs > 0u) && (ctlr->delay_us != NULL))
        {
            ctlr->delay_us(ctlr->delay_ctx, xfer->delay_usecs);
        }

        /* cs_change：本段结束后放开片选，下一段重新拉低（最后一段不必，
         * 反正循环结束会统一释放） */
        if ((xfer->cs_change != 0u) && ((i + 1u) < msg->n))
        {
            spi_cs(ctlr, spi, 0);
            spi_cs(ctlr, spi, 1);
        }
    }

    spi_cs(ctlr, spi, 0);

out:
    spi_bus_unlock(ctlr);
    msg->status = ret;
    return ret;
}

/* ============================ 单段便捷接口 ============================ */

int spi_write(struct spi_device *spi, const void *buf, uint16_t len)
{
    struct spi_message msg;
    struct spi_transfer xfer;

    if (buf == NULL || len == 0u)
    {
        return SPI_ERR_PARAM;
    }

    xfer.tx_buf = buf;
    xfer.rx_buf = NULL;
    xfer.len = len;
    xfer.speed_hz = 0u;
    xfer.delay_usecs = 0u;
    xfer.cs_change = 0u;
    xfer.bits_per_word = 0u;

    spi_message_init(&msg);
    (void)spi_message_add_tail(&xfer, &msg);

    return spi_sync(spi, &msg);
}

int spi_read(struct spi_device *spi, void *buf, uint16_t len)
{
    struct spi_message msg;
    struct spi_transfer xfer;

    if (buf == NULL || len == 0u)
    {
        return SPI_ERR_PARAM;
    }

    xfer.tx_buf = NULL;
    xfer.rx_buf = buf;
    xfer.len = len;
    xfer.speed_hz = 0u;
    xfer.delay_usecs = 0u;
    xfer.cs_change = 0u;
    xfer.bits_per_word = 0u;

    spi_message_init(&msg);
    (void)spi_message_add_tail(&xfer, &msg);

    return spi_sync(spi, &msg);
}

int spi_write_then_read(struct spi_device *spi,
                        const void *txbuf, uint16_t n_tx,
                        void *rxbuf, uint16_t n_rx)
{
    struct spi_message msg;
    struct spi_transfer xfer[2];
    unsigned int n = 0u;

    if (((txbuf == NULL) || (n_tx == 0u)) && ((rxbuf == NULL) || (n_rx == 0u)))
    {
        return SPI_ERR_PARAM;
    }

    spi_message_init(&msg);

    if ((txbuf != NULL) && (n_tx > 0u))
    {
        xfer[n].tx_buf = txbuf;
        xfer[n].rx_buf = NULL;
        xfer[n].len = n_tx;
        xfer[n].speed_hz = 0u;
        xfer[n].delay_usecs = 0u;
        xfer[n].cs_change = 0u;
        xfer[n].bits_per_word = 0u;
        if (spi_message_add_tail(&xfer[n], &msg) != 0)
        {
            return SPI_ERR_NOMEM;
        }
        n++;
    }

    if ((rxbuf != NULL) && (n_rx > 0u))
    {
        xfer[n].tx_buf = NULL;
        xfer[n].rx_buf = rxbuf;
        xfer[n].len = n_rx;
        xfer[n].speed_hz = 0u;
        xfer[n].delay_usecs = 0u;
        xfer[n].cs_change = 0u;
        xfer[n].bits_per_word = 0u;
        if (spi_message_add_tail(&xfer[n], &msg) != 0)
        {
            return SPI_ERR_NOMEM;
        }
        n++;
    }

    return spi_sync(spi, &msg);
}
