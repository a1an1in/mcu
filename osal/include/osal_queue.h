/*
 * osal_queue.h - 消息队列抽象接口
 */
#ifndef OSAL_QUEUE_H__
#define OSAL_QUEUE_H__

#include "osal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 消息队列句柄 */
typedef void *osal_queue_t;

/*
 * 创建消息队列。
 * @param item_size 每条消息的大小（字节）
 * @param item_count 队列容量（条数）
 * @param q 输出句柄
 */
osal_err_t osal_queue_create(osal_queue_t *q, uint32_t item_size, uint32_t item_count);

/*
 * 向队列发送一条消息（拷贝 item_size 字节）。
 * timeout_ms 为队列满时的阻塞时间。
 * @return OSAL_OK / OSAL_ETIMEDOUT / OSAL_EINVAL
 */
osal_err_t osal_queue_send(osal_queue_t q, const void *item, uint32_t timeout_ms);

/*
 * 从队列接收一条消息，存入 buf。timeout_ms 为队列空时的阻塞时间。
 * @return OSAL_OK / OSAL_ETIMEDOUT / OSAL_EINVAL
 */
osal_err_t osal_queue_receive(osal_queue_t q, void *buf, uint32_t timeout_ms);

/* 删除消息队列 */
void osal_queue_delete(osal_queue_t q);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_QUEUE_H__ */