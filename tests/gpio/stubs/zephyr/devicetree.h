/*
 * zephyr/devicetree.h - Zephyr <zephyr/devicetree.h> 的 host 测试桩。
 * DT_NODELABEL 解析为对应端口桩对象符号，DEVICE_DT_GET 取其地址。
 */
#ifndef STUB_ZEPHYR_DEVICETREE_H__
#define STUB_ZEPHYR_DEVICETREE_H__

#define DT_NODELABEL(name)  __gpio_dev_##name
#define DEVICE_DT_GET(id)   (&(id))

#endif /* STUB_ZEPHYR_DEVICETREE_H__ */