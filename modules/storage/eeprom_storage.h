/*
 * eeprom_storage.h - EEPROM 上的 key-value 配置存储 <modules/storage/>
 *
 * 定位：把一小堆「字符串配置」持久化到板载 EEPROM（如 24C02），
 *       掉电不丢。典型用途：WiFi 账号、设备名、阈值参数。
 *
 * 依赖：
 *   - OSAL（osal_mutex / osal_malloc / osal_printf）—— 与 RTOS 解耦；
 *   - 字节级接口 eeprom.h —— 与 MCU / 总线 / 器件解耦。
 *   不 include 任何 OS 头、不碰寄存器，也**不认识** AT24C02。
 *
 * ── 接口形状 ──────────────────────────────────────────────────────────
 * 用「函数表 + 句柄」的写法，调用方只通过函数指针操作对象：
 *
 *     struct at24c02_dev chip;
 *     struct eeprom_dev rom;
 *
 *     at24c02_init(&chip, i2c_get_adapter(0), 0x50);
 *     at24c02_register(&rom, &chip, 256);   // 板级负责这一步
 *
 *     eeprom_storage_t *es = eeprom_storage_create();
 *     es->init(es, &rom);                   // 只绑定后端，不读写
 *     es->load(es);                         // 从后端读出配置到缓存
 *
 *     es->set_value(es, "ssid", "test");    // 只写缓存
 *     char *v = es->get_value(es, "ssid");  // 从缓存读（返回堆字符串）
 *     osal_free(v);
 *
 *     es->save(es);                         // 把缓存写回后端
 *     eeprom_storage_destroy(es);
 *
 * 为什么需要 create：init 本身也是函数表里的一项，没法自己把自己填上，
 * 所以由 create 负责分配对象并把 5 个函数指针填好。
 *
 * ── 三个容易踩的语义 ──────────────────────────────────────────────────
 * 1) set_value 只动缓存，不写 ROM。要落盘必须再调 save。
 *    反过来，save 之后缓存依然有效，可以继续 set / get。
 *
 * 2) set_value / get_value 在缓存尚未加载时会先自动 load 一次。
 *    这是刻意的：如果允许在「只知道自己设过的几个 key」的半空缓存上 save，
 *    就会把 ROM 里其它 key 全部抹掉。自动 load 一次，代价是一次 EEPROM 读，
 *    换来的是「save 永远不会误删别人的数据」。
 *
 * 3) get_value 返回 osal_malloc 出来的字符串，所有权归调用方；
 *    查不到 key（或加载失败 / 内存不足）返回 NULL。
 *    注意 NULL 同时表示这几种情况，需要区分时请先显式调 load()。
 *
 * ── 数据在 ROM 上的布局 ───────────────────────────────────────────────
 *     [0]      magic 'E' (0x45)
 *     [1]      版本号，当前 1
 *     [2..3]   正文长度（小端 16 位）
 *     [4]      正文的 CRC-8（多项式 0x07，初值 0，不反转）
 *     [5..]    正文：一个 JSON 对象
 *
 *     存 ssid=test、port=8080 两项，正文就是 25 个字节：
 *         {"ssid":"test","port":8080}
 *
 * 写的时候先写正文、最后写头部 5 字节 —— 头部固定落在 EEPROM 第 0 页
 * （0x00~0x04），一次页写就完成，等于一个「提交点」。如果正文写到一半掉电，
 * 头部还是旧的，长度/CRC 对不上，load() 会判定为数据损坏，而不是读出半截
 * 脏数据把配置搞乱。
 *
 * ── JSON 语法约束（够用即可，不做通用解析器）──────────────────────────
 *   - 只支持一层对象。嵌套的 {} / [] 一律拒绝 —— 这套接口本来就是扁平的；
 *   - 键必须是字符串；
 *   - 值可以是字符串，也可以是数字 / true / false / null。后者按**字面量**
 *     原样保存并原样写回：手工写的 {"port":8080} 保存后仍是 8080，
 *     不会变成 "8080"。走 set_value() 写入的值则一律是字符串；
 *   - 转义支持 \" \\ \/ \b \f \n \r \t 和 \uXXXX；\u0000 与代理对不支持
 *     （缓存是 C 字符串，装不下 NUL，也没有 UTF-16 成对处理）；
 *   - 键名重复时以最后一次出现为准，保存后只留一份。
 *
 * ── 能力边界 ─────────────────────────────────────────────────────────
 *   - 值对外一律是 C 字符串：数字请调用方自己 atoi（JSON 里的 8080 读出来
 *     是 "8080"，类型信息只在落盘时保留）；
 *   - 没有删除接口，用 set_value(es, key, "") 把值置空即可；
 *   - 掉电保护只到「检测出损坏」这一级，没有 A/B 双备份。
 */
#ifndef MODULE_EEPROM_STORAGE_H__
#define MODULE_EEPROM_STORAGE_H__

#include <osal.h>

#include "eeprom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct eeprom_storage eeprom_storage_t;

struct eeprom_storage {
    /* ---- 对外接口（由 create 填好，调用方直接 es->xxx(...)）---- */

    /* 绑定底层 EEPROM（eeprom.h 的字节级接口）。
     *   dev  设备句柄，须已由器件注册函数（如 at24c02_register()）或
     *        eeprom_register() 构造好（eeprom_dev_valid() 为真，
     *        capacity 能放下 5 字节头部 + 正文）。
     * 返回 OSAL_OK / OSAL_EINVAL（dev 为空、函数指针为空、容量非法）。
     * 只做绑定，不读写器件；可重复调用以切换器件。
     * 本模块只记 dev 的指针，调用方须保证它在实例存活期间一直有效。 */
    int   (*init)(eeprom_storage_t *es, struct eeprom_dev *dev);

    /* 写缓存。key 不能为空串；value 可为空串。
     * 值没变化时直接返回 OSAL_OK 且不置脏标记（不会产生无谓的 EEPROM 擦写）。
     * 返回 OSAL_OK / OSAL_EINVAL / OSAL_ENOMEM / ES_ERR_NOSPACE / OSAL_EIO。 */
    int   (*set_value)(eeprom_storage_t *es, const char *key, const char *value);

    /* 读缓存，返回堆字符串（调用方 osal_free）；key 不存在返回 NULL。 */
    char *(*get_value)(eeprom_storage_t *es, const char *key);

    /* 从 EEPROM 读配置并重建缓存。会覆盖缓存里已有的内容。
     * 返回 OSAL_OK / OSAL_EIO / OSAL_ENOMEM / ES_ERR_FORMAT。 */
    int   (*load)(eeprom_storage_t *es);

    /* 把缓存写回 EEPROM。
     * 没有改动时直接返回 OSAL_OK，一个字节都不写（省 EEPROM 寿命）。
     * 返回 OSAL_OK / OSAL_EIO / OSAL_ENOMEM / ES_ERR_NOSPACE / ES_ERR_FORMAT。 */
    int   (*save)(eeprom_storage_t *es);

    /* ---- 以下是私有状态，调用方不要碰 ---- */
    void *priv;
};

/* 创建 / 销毁句柄。create 只分配并填函数表，后端绑定要再调 es->init()。 */
eeprom_storage_t *eeprom_storage_create(void);
void eeprom_storage_destroy(eeprom_storage_t *es);

/* ---- 默认实例 ----
 * 板级初始化时用 set 注册「全系统共享的那一份配置」，msh 等模块用 get 取用。
 * 只保存指针，不接管生命周期：销毁前记得先 set(NULL)，destroy 也会自动清掉
 * 自己注册的默认实例。未注册时 get 返回 NULL。 */
void eeprom_storage_set_default(eeprom_storage_t *es);
eeprom_storage_t *eeprom_storage_get_default(void);

/* ============================ 本模块追加的错误码 ============================
 * 前 6 个沿用 osal_err_t（0 / -1 / -2 / -3 / -4 / -5 / -6），这里从 -7 起续。
 */
#define ES_ERR_NOSPACE   (-7)   /* 序列化后超过 EEPROM 可用容量 */
#define ES_ERR_FORMAT    (-8)   /* ROM 里的数据不合法（长度/CRC/行格式） */
#define ES_ERR_VERIFY    (-9)   /* 写后回读校验不过：器件 ACK 了但内容没落盘 */

#ifdef __cplusplus
}
#endif

#endif /* MODULE_EEPROM_STORAGE_H__ */
