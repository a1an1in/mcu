/*
 * hello.h - 示例模块：周期性打印（验证 OSAL 线程/打印链路）
 */
#ifndef MODULE_HELLO_H__
#define MODULE_HELLO_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 hello 演示线程（必须在板级初始化完成后调用） */
void hello_start(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_HELLO_H__ */