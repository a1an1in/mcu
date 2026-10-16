/*
 * zephyr/device.h - Zephyr <zephyr/device.h> 的 host 测试桩。
 * 适配实现只解引用 const struct device* 指针，因此桩中仅需占位结构。
 */
#ifndef STUB_ZEPHYR_DEVICE_H__
#define STUB_ZEPHYR_DEVICE_H__

struct device { int _dummy; };

#endif /* STUB_ZEPHYR_DEVICE_H__ */