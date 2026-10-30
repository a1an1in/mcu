/*
 * sdio.h - SDIO 主机控制器抽象（SoC 层接口） <drivers/sd/>
 *
 * 这一层只回答「怎么把一条命令/一块数据搬到总线上」，不认识 CMD0/ACMD41
 * 这些协议——协议在 sd.c。换 MCU 只换 stm32f1_sdio.c。
 *
 * 分层：
 *   sd.h / sd.c         卡协议（初始化状态机、CID/CSD、块读写）
 *   sdio.h              本文件：主机控制器接口
 *   stm32f1_sdio.c      STM32F1 SDIO 寄存器实现
 */
#ifndef DRIVERS_SDIO_H__
#define DRIVERS_SDIO_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 响应类型（对应 STM32 SDIO_CMD.WAITRESP，别直接拿它当位域用） */
#define SDIO_RESP_NONE      0   /* 无响应（CMD0/CMD12 之外的广播类） */
#define SDIO_RESP_SHORT     1   /* 48 位短响应：R1 / R3 / R6 / R7 */
#define SDIO_RESP_LONG      2   /* 136 位长响应：R2（CID/CSD） */

/* 与上面组合使用：**不检查响应 CRC**。
 * 必须用在 R3（ACMD41/CMD1 的 OCR）：这个响应按 SD 规范根本没有 CRC7 字段，
 * STM32 的 SDIO 硬件却无条件校验，于是 CCRCFAIL 必然置位 —— 但那不是错误，
 * 响应内容（RESP1）是好的。不忽略它，ACMD41 会永远「CRC 错」。 */
#define SDIO_RESP_NO_CRC    0x80

struct sdio_host;

struct sdio_host_ops {
    /* 发一条命令并等响应。
     *   resp_type  SDIO_RESP_*
     *   resp       4 个字，按 R2 的顺序存放（resp[0] 是最高 32 位）；
     *              无响应时可以是 NULL。
     * 成功 0；超时 / CRC 错返回负值。 */
    int (*send_cmd)(struct sdio_host *h, uint8_t cmd, uint32_t arg,
                    uint8_t resp_type, uint32_t resp[4]);

    /* 设置 SD 时钟（Hz）。实现应取「不超过该值」的最接近分频。 */
    int (*set_clock)(struct sdio_host *h, uint32_t hz);

    /* 1 = 1 位总线，4 = 4 位总线 */
    int (*set_bus_width)(struct sdio_host *h, unsigned int width);

    /* 数据阶段准备：设置长度/块大小/方向、启动数据状态机，并（写方向）
     * 把 tx 开头的一段预填进 FIFO。
     * 必须在发 CMD17/18/24/25 **之前**调用 —— 卡在响应之后立刻就要数据，
     * 等命令返回再填就来不及（TXUNDERR）。 */
    int (*data_start)(struct sdio_host *h, int read, const void *tx,
                      uint32_t total_bytes, uint32_t blk_size);

    /* 搬数据（阻塞到搬完）。read = 1 从卡读进 buf，0 从 buf 写到卡。 */
    int (*data_move)(struct sdio_host *h, int read,
                     void *buf, uint32_t total_bytes);
};

struct sdio_host {
    const struct sdio_host_ops *ops;
    void *priv;                 /* SoC 层私有数据（寄存器基址、时钟等） */
};

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_SDIO_H__ */
