/*
 * eeprom_storage.c - EEPROM key-value 配置存储实现 <modules/storage/>
 *
 * 结构（自上而下）：
 *   1. 私有状态与 kv 链表
 *   2. 字符串小工具
 *   3. JSON：转义 / 反转义 / 序列化 / 解析（只支持一层对象）
 *   4. ROM 布局：load / save（含 magic / 长度 / CRC-8）
 *   5. 对外接口：init / set_value / get_value / load / save + create / destroy
 *
 * 一条设计主线：**缓存在内存里是链表，JSON 只是它在 EEPROM 上的序列化格式**。
 * 所以 set_value 不需要重建整串 JSON、也不会因为临时超长而失败；
 * 只有 save 那一刻才计算总长度并检查容量。
 */
#include <string.h>

#include <osal.h>

#include "eeprom_storage.h"

/* ============================ 1. 私有状态 ============================ */

/* ROM 头部：[magic][ver][len_lo][len_hi][crc8]，共 5 字节 */
#define ES_MAGIC        0x45u   /* 'E' */
#define ES_VERSION      0x01u
#define ES_HDR_LEN      5u

/* 正文可用容量 = 总容量 - 头部 */
#define ES_PAYLOAD_CAP(p)   ((p)->size - ES_HDR_LEN)

struct es_entry {
    struct es_entry *next;
    char *key;
    char *value;
    /* 1 = 原文是 JSON 字符串（"8080"），0 = 裸字面量（8080 / true / null）。
     * 分开记是为了原样写回：别人写的数字不会被我们偷偷改成字符串。 */
    uint8_t quoted;
};

struct es_priv {
    struct eeprom_dev *dev;     /* 底层 EEPROM 句柄（由 eeprom_register 或
                                 * 器件适配的 xxx_attach 构造好） */
    uint32_t size;              /* 器件容量（字节），init 时从 dev 抄一份 */
    struct es_entry *head;      /* 缓存：kv 链表，尾部插入保持设置顺序 */
    osal_mutex_t lock;          /* 保护上面这些字段 */
    uint8_t loaded;             /* 缓存是否已经从 ROM 加载过 */
    uint8_t dirty;              /* 缓存相对 ROM 是否有改动 */
};

/* ============================ 2. 字符串小工具 ============================ */

static char *es_strndup(const char *s, size_t n)
{
    char *d = (char *)osal_malloc(n + 1u);

    if (d == OSAL_NULL)
    {
        return OSAL_NULL;
    }

    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char *es_strdup(const char *s)
{
    return es_strndup(s, strlen(s));
}

static void es_list_free(struct es_entry *e)
{
    while (e != OSAL_NULL)
    {
        struct es_entry *next = e->next;

        osal_free(e->key);
        osal_free(e->value);
        osal_free(e);
        e = next;
    }
}

static struct es_entry *es_find(struct es_entry *head, const char *key)
{
    while (head != OSAL_NULL)
    {
        if (strcmp(head->key, key) == 0)
        {
            return head;
        }
        head = head->next;
    }
    return OSAL_NULL;
}

/* ============================ 3. JSON ============================ */

/* 转义后的长度：普通字节 1，\n 之类的两字节转义 2，其它控制字符 \u00XX 占 6 */
static uint32_t json_escaped_len(const char *s)
{
    uint32_t n = 0u;

    for (; *s != '\0'; s++)
    {
        unsigned char c = (unsigned char)*s;

        if (c == '"' || c == '\\' || c == '\n' || c == '\r' ||
            c == '\t' || c == '\b' || c == '\f')
        {
            n += 2u;
        }
        else if (c < 0x20u)
        {
            n += 6u;
        }
        else
        {
            n += 1u;
        }
    }
    return n;
}

/* 把 s 转义写进 out（不加引号、不写 NUL），返回写入字节数。
 * 调用方需保证容量 ≥ json_escaped_len(s)。 */
static uint32_t json_escape(char *out, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    char *o = out;

    for (; *s != '\0'; s++)
    {
        unsigned char c = (unsigned char)*s;

        switch (c)
        {
        case '"':  *o++ = '\\'; *o++ = '"';  break;
        case '\\': *o++ = '\\'; *o++ = '\\'; break;
        case '\n': *o++ = '\\'; *o++ = 'n';  break;
        case '\r': *o++ = '\\'; *o++ = 'r';  break;
        case '\t': *o++ = '\\'; *o++ = 't';  break;
        case '\b': *o++ = '\\'; *o++ = 'b';  break;
        case '\f': *o++ = '\\'; *o++ = 'f';  break;
        default:
            if (c < 0x20u)
            {
                *o++ = '\\';
                *o++ = 'u';
                *o++ = '0';
                *o++ = '0';
                *o++ = hex[(c >> 4) & 0x0Fu];
                *o++ = hex[c & 0x0Fu];
            }
            else
            {
                *o++ = (char)c;
            }
            break;
        }
    }
    return (uint32_t)(o - out);
}

static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
        p++;
    }
    return p;
}

static int json_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 解析一个 JSON 字符串字面量（p 指向开引号），成功时 *out 为 malloc 出的
 * 反转义结果、*pp 指向闭引号之后。返回 0 成功，-1 失败。 */
static int json_parse_string(const char **pp, char **out)
{
    const char *p = *pp + 1;
    char *buf;
    char *o;

    /* 反转义只会变短，按「剩余全文长度 + 1」分配一定够 */
    buf = (char *)osal_malloc(strlen(p) + 1u);
    if (buf == OSAL_NULL)
    {
        return -1;
    }
    o = buf;

    for (;;)
    {
        unsigned char c = (unsigned char)*p;

        if (c == '\0')
        {
            osal_free(buf);
            return -1;
        }
        if (c == '"')
        {
            p++;
            break;
        }
        if (c != '\\')
        {
            *o++ = (char)c;
            p++;
            continue;
        }

        p++;
        switch (*p)
        {
        case '"':  *o++ = '"';  p++; break;
        case '\\': *o++ = '\\'; p++; break;
        case '/':  *o++ = '/';  p++; break;
        case 'b':  *o++ = '\b'; p++; break;
        case 'f':  *o++ = '\f'; p++; break;
        case 'n':  *o++ = '\n'; p++; break;
        case 'r':  *o++ = '\r'; p++; break;
        case 't':  *o++ = '\t'; p++; break;
        case 'u':
        {
            unsigned int cp = 0u;
            int i;

            p++;
            for (i = 0; i < 4; i++)
            {
                int h = json_hexval(p[i]);

                if (h < 0)
                {
                    osal_free(buf);
                    return -1;
                }
                cp = (cp << 4) | (unsigned int)h;
            }
            p += 4;

            if (cp == 0u || (cp >= 0xD800u && cp <= 0xDFFFu))
            {
                /* \u0000 放不进 C 字符串；代理对需要成对处理，本模块不支持 */
                osal_free(buf);
                return -1;
            }

            if (cp < 0x80u)
            {
                *o++ = (char)cp;
            }
            else if (cp < 0x800u)
            {
                *o++ = (char)(0xC0u | (cp >> 6));
                *o++ = (char)(0x80u | (cp & 0x3Fu));
            }
            else
            {
                *o++ = (char)(0xE0u | (cp >> 12));
                *o++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                *o++ = (char)(0x80u | (cp & 0x3Fu));
            }
            break;
        }
        default:
            osal_free(buf);
            return -1;
        }
    }

    *o = '\0';
    *pp = p;
    *out = buf;
    return 0;
}

/* 一项在 JSON 里占多少字节，**不含**分隔逗号：
 *   "key":"value"  →  key + value + 5
 *   "key":value    →  key + value + 3   （裸字面量：数字 / true / false / null） */
static uint32_t json_entry_len_raw(const char *key, const char *value, uint8_t quoted)
{
    uint32_t n = json_escaped_len(key) + 2u + 1u;    /* "key" : */

    if (quoted != 0u)
    {
        n += json_escaped_len(value) + 2u;           /* "value" */
    }
    else
    {
        n += (uint32_t)strlen(value);                /* 原样 */
    }
    return n;
}

static uint32_t json_entry_len(const struct es_entry *e)
{
    return json_entry_len_raw(e->key, e->value, e->quoted);
}

/* 整个对象的长度：两个花括号 + 各项 + (项数-1) 个逗号 */
static uint32_t json_total_len(const struct es_entry *head)
{
    uint32_t n = 2u;    /* { } */
    int first = 1;

    for (; head != OSAL_NULL; head = head->next)
    {
        if (first == 0)
        {
            n += 1u;    /* 逗号 */
        }
        first = 0;
        n += json_entry_len(head);
    }
    return n;
}

/* 假设把 key 换成 value 之后整个对象多长（key 不存在就是新增一项）。
 * set_value 用它做容量预检 —— 必须先算再改，不能改完再回滚。 */
static uint32_t json_total_if_set(const struct es_entry *head,
                                  const char *key, const char *value)
{
    uint32_t n = 2u;
    int first = 1;
    int hit = 0;

    for (; head != OSAL_NULL; head = head->next)
    {
        if (first == 0)
        {
            n += 1u;
        }
        first = 0;

        if (hit == 0 && strcmp(head->key, key) == 0)
        {
            n += json_entry_len_raw(key, value, 1u);    /* 覆盖：仍按字符串 */
            hit = 1;
        }
        else
        {
            n += json_entry_len(head);
        }
    }

    if (hit == 0)
    {
        if (first == 0)
        {
            n += 1u;                                    /* 新增前要补个逗号 */
        }
        n += json_entry_len_raw(key, value, 1u);
    }
    return n;
}

/* 序列化成 JSON 对象。容量不足返回 ES_ERR_NOSPACE。 */
static int json_serialize(const struct es_entry *head, char *out,
                          uint32_t cap, uint32_t *out_len)
{
    const struct es_entry *e;
    char *o = out;

    if (json_total_len(head) > cap)
    {
        return ES_ERR_NOSPACE;
    }

    *o++ = '{';
    for (e = head; e != OSAL_NULL; e = e->next)
    {
        if (e != head)
        {
            *o++ = ',';
        }
        *o++ = '"';
        o += json_escape(o, e->key);
        *o++ = '"';
        *o++ = ':';

        if (e->quoted != 0u)
        {
            *o++ = '"';
            o += json_escape(o, e->value);
            *o++ = '"';
        }
        else
        {
            size_t vlen = strlen(e->value);

            memcpy(o, e->value, vlen);
            o += vlen;
        }
    }
    *o++ = '}';

    *out_len = (uint32_t)(o - out);
    return OSAL_OK;
}

/* 解析一层 JSON 对象，成功时 *out 为链表头（由调用方负责释放）。
 *
 * 值的处理：带引号的按字符串反转义并标记 quoted=1；不带引号的（数字 / true /
 * false / null）按原文存成文本并标记 quoted=0，这样 save 时能原样写回去。
 * 遇到嵌套的 { 或 [ 直接报格式错误 —— 本模块只存一层扁平配置。 */
static int json_parse(const char *json, struct es_entry **out)
{
    const char *p = json;
    struct es_entry *head = OSAL_NULL;
    struct es_entry *tail = OSAL_NULL;
    int ret = OSAL_OK;

    p = json_skip_ws(p);
    if (*p != '{')
    {
        return ES_ERR_FORMAT;
    }
    p++;

    for (;;)
    {
        char *key = OSAL_NULL;
        char *value = OSAL_NULL;
        uint8_t quoted = 1u;
        struct es_entry *e;

        p = json_skip_ws(p);

        if (*p == '}')
        {
            p++;
            break;
        }
        if (*p != '"')
        {
            ret = ES_ERR_FORMAT;
            goto out;
        }
        if (json_parse_string(&p, &key) != 0)
        {
            ret = ES_ERR_FORMAT;
            goto out;
        }

        p = json_skip_ws(p);
        if (*p != ':')
        {
            osal_free(key);
            ret = ES_ERR_FORMAT;
            goto out;
        }
        p = json_skip_ws(p + 1);

        if (*p == '{' || *p == '[')
        {
            osal_free(key);
            ret = ES_ERR_FORMAT;      /* 不支持嵌套 */
            goto out;
        }

        if (*p == '"')
        {
            if (json_parse_string(&p, &value) != 0)
            {
                osal_free(key);
                ret = ES_ERR_FORMAT;
                goto out;
            }
        }
        else
        {
            const char *s = p;

            while (*p != '\0' && *p != ',' && *p != '}' &&
                   *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            {
                p++;
            }
            if (p == s)
            {
                osal_free(key);
                ret = ES_ERR_FORMAT;
                goto out;
            }
            value = es_strndup(s, (size_t)(p - s));
            if (value == OSAL_NULL)
            {
                osal_free(key);
                ret = OSAL_ENOMEM;
                goto out;
            }
            quoted = 0u;
        }

        /* 重复键：以最后一次出现为准，链表里只留一项 */
        e = es_find(head, key);
        if (e != OSAL_NULL)
        {
            osal_free(e->value);
            e->value = value;
            e->quoted = quoted;
            osal_free(key);
        }
        else
        {
            e = (struct es_entry *)osal_malloc(sizeof(*e));
            if (e == OSAL_NULL)
            {
                osal_free(key);
                osal_free(value);
                ret = OSAL_ENOMEM;
                goto out;
            }
            e->next = OSAL_NULL;
            e->key = key;
            e->value = value;
            e->quoted = quoted;

            if (tail != OSAL_NULL)
            {
                tail->next = e;
            }
            else
            {
                head = e;
            }
            tail = e;
        }

        p = json_skip_ws(p);
        if (*p == ',')
        {
            p++;
            continue;
        }
        if (*p == '}')
        {
            p++;
            break;
        }
        ret = ES_ERR_FORMAT;
        goto out;
    }

    /* 后面只允许空白，否则视为脏数据 */
    p = json_skip_ws(p);
    if (*p != '\0')
    {
        ret = ES_ERR_FORMAT;
    }

out:
    if (ret != OSAL_OK)
    {
        es_list_free(head);
        return ret;
    }

    *out = head;
    return OSAL_OK;
}

/* ============================ 4. ROM 布局 ============================ */

/* CRC-8：多项式 0x07、初值 0、不反转（与 SMBus PEC 同一套参数）。
 * 这里单独实现而不是去用 smbus.h —— 本模块不该反向依赖 SMBus 协议层。 */
static uint8_t es_crc8(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0u;

    while (len-- > 0u)
    {
        int bit;

        crc ^= *data++;
        for (bit = 0; bit < 8; bit++)
        {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void es_cache_clear(struct es_priv *p)
{
    es_list_free(p->head);
    p->head = OSAL_NULL;
    p->dirty = 0u;
}

/* 调用前必须已持有 p->lock。返回 OSAL_OK 时 p->loaded = 1。
 *
 * 三种结果要分开：
 *   - 空片 / 版本不符 → 当成「出厂状态」，清空缓存，返回 OSAL_OK；
 *   - 读总线失败      → 不动 loaded，下次调用会重试（可能是瞬时干扰）；
 *   - 数据损坏        → 清空缓存并置 loaded，避免每次 get 都去读一遍坏数据。
 */
static int es_load_locked(struct es_priv *p)
{
    uint8_t hdr[ES_HDR_LEN];
    uint8_t *buf;
    uint32_t len;
    struct es_entry *head = OSAL_NULL;
    int ret;

    if (eeprom_read(p->dev, 0u, hdr, ES_HDR_LEN) != EEPROM_OK)
    {
        return OSAL_EIO;
    }

    if (hdr[0] != ES_MAGIC || hdr[1] != ES_VERSION)
    {
        /* 新片子（全 0xFF）或换过版本：按空配置处理，不报错 */
        es_cache_clear(p);
        p->loaded = 1u;
        return OSAL_OK;
    }

    len = (uint32_t)hdr[2] | ((uint32_t)hdr[3] << 8);
    if ((len + ES_HDR_LEN) > p->size)
    {
        ret = ES_ERR_FORMAT;
        goto corrupt;
    }

    if (len == 0u)
    {
        /* 头部合法但正文为空：就是一份空配置 */
        es_cache_clear(p);
        p->loaded = 1u;
        return OSAL_OK;
    }

    buf = (uint8_t *)osal_malloc(len + 1u);
    if (buf == OSAL_NULL)
    {
        return OSAL_ENOMEM;
    }

    if (eeprom_read(p->dev, ES_HDR_LEN, buf, (size_t)len) != EEPROM_OK)
    {
        osal_free(buf);
        return OSAL_EIO;
    }
    buf[len] = '\0';

    if (es_crc8(buf, len) != hdr[4])
    {
        osal_free(buf);
        ret = ES_ERR_FORMAT;
        goto corrupt;
    }

    ret = json_parse((const char *)buf, &head);
    osal_free(buf);
    if (ret != OSAL_OK)
    {
        ret = (ret == OSAL_ENOMEM) ? ret : ES_ERR_FORMAT;
        goto corrupt;
    }

    es_cache_clear(p);
    p->head = head;
    p->loaded = 1u;
    return OSAL_OK;

corrupt:
    osal_printf("[storage] ROM 数据损坏（err=%d），缓存按空配置处理\n", ret);
    es_cache_clear(p);
    p->loaded = 1u;
    return ret;
}

/* 调用前必须已持有 p->lock。先把正文写进去，最后写 5 字节头部作提交点。
 * 头部固定落在 EEPROM 第 0 页（0x00~0x04），一次页写完成。 */
static int es_save_locked(struct es_priv *p)
{
    uint8_t hdr[ES_HDR_LEN];
    uint8_t *buf;
    uint32_t len = 0u;
    uint32_t cap;
    int ret;

    if (p->dirty == 0u)
    {
        return OSAL_OK;             /* 没有改动，一个字节都不写 */
    }

    cap = ES_PAYLOAD_CAP(p);
    buf = (uint8_t *)osal_malloc(cap);
    if (buf == OSAL_NULL)
    {
        return OSAL_ENOMEM;
    }

    ret = json_serialize(p->head, (char *)buf, cap, &len);
    if (ret != OSAL_OK)
    {
        osal_free(buf);
        return ret;
    }

    hdr[0] = ES_MAGIC;
    hdr[1] = ES_VERSION;
    hdr[2] = (uint8_t)(len & 0xFFu);
    hdr[3] = (uint8_t)((len >> 8) & 0xFFu);
    hdr[4] = es_crc8(buf, len);

    /* 正文为空时器件层会拒绝 len=0，跳过即可 */
    ret = EEPROM_OK;
    if (len > 0u)
    {
        ret = eeprom_write(p->dev, ES_HDR_LEN, buf, (size_t)len);
    }
    if (ret == EEPROM_OK)
    {
        ret = eeprom_write(p->dev, 0u, hdr, ES_HDR_LEN);
    }
    osal_free(buf);

    if (ret != EEPROM_OK)
    {
        return OSAL_EIO;            /* dirty 保持不变，下次还能重试 */
    }

    p->dirty = 0u;
    return OSAL_OK;
}

/* ============================ 5. 对外接口 ============================ */

static int es_init(eeprom_storage_t *es, struct eeprom_dev *dev)
{
    struct es_priv *p;

    if (es == OSAL_NULL || es->priv == OSAL_NULL || dev == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }
    /* 只认「可读可写 + 容量非 0」的句柄，不再逐个判函数指针 */
    if (eeprom_dev_valid(dev) == 0)
    {
        return OSAL_EINVAL;
    }
    /* 头部之外至少要能放下 "{}"；正文长度字段只有 16 位，超了没法编码 */
    if (dev->capacity <= ES_HDR_LEN ||
        dev->capacity > (0xFFFFu + ES_HDR_LEN))
    {
        return OSAL_EINVAL;
    }

    p = (struct es_priv *)es->priv;

    if (p->lock == OSAL_NULL)
    {
        if (osal_mutex_create(&p->lock) != OSAL_OK)
        {
            return OSAL_ENOMEM;
        }
    }

    /* 注意：这里刻意不加锁。
     * init 只做「绑后端 + 清缓存」，调用方（板级装配）本来就是单线程；
     * 更要紧的是它可能跑在调度器启动之前（RT-Thread 的 rt_hw_board_init 早于
     * rt_system_scheduler_init），那时 rt_mutex_take() 拿不到 rt_thread_self()，
     * 会往野地址写 TCB 字段，表现成 BusFault/IMPRECISERR 直接死机。
     * 互斥量本身在这里创建是安全的（纯对象分配，不依赖当前线程）。 */
    p->dev = dev;
    p->size = dev->capacity;
    es_cache_clear(p);
    p->loaded = 0u;

    return OSAL_OK;
}

static int es_set_value(eeprom_storage_t *es, const char *key, const char *value)
{
    struct es_priv *p;
    struct es_entry *e;
    struct es_entry *tail;
    int ret;

    if (es == OSAL_NULL || es->priv == OSAL_NULL ||
        key == OSAL_NULL || value == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }
    if (key[0] == '\0')
    {
        return OSAL_EINVAL;
    }

    p = (struct es_priv *)es->priv;
    if (p->lock == OSAL_NULL)
    {
        return OSAL_EINVAL;         /* 还没 init */
    }
    if (osal_mutex_lock(p->lock, OSAL_WAIT_FOREVER) != OSAL_OK)
    {
        return OSAL_EBUSY;
    }

    /* 先补一次 load：否则在「只有自己设过的 key」的半空缓存上 save
     * 会把 ROM 里其它 key 全部抹掉 */
    if (p->loaded == 0u)
    {
        ret = es_load_locked(p);
        if (ret != OSAL_OK)
        {
            osal_mutex_unlock(p->lock);
            return ret;
        }
    }

    e = es_find(p->head, key);
    if (e != OSAL_NULL && e->quoted != 0u && strcmp(e->value, value) == 0)
    {
        osal_mutex_unlock(p->lock);
        return OSAL_OK;             /* 值没变，不置脏，不产生 EEPROM 擦写 */
    }

    /* 容量预检：先算出「设完之后有多长」，超了就直接拒绝，不改缓存 */
    if (json_total_if_set(p->head, key, value) > ES_PAYLOAD_CAP(p))
    {
        osal_mutex_unlock(p->lock);
        return ES_ERR_NOSPACE;
    }

    if (e != OSAL_NULL)
    {
        char *nv = es_strdup(value);

        if (nv == OSAL_NULL)
        {
            osal_mutex_unlock(p->lock);
            return OSAL_ENOMEM;
        }
        osal_free(e->value);
        e->value = nv;
        e->quoted = 1u;             /* 走 API 写进去的一律是字符串 */
    }
    else
    {
        e = (struct es_entry *)osal_malloc(sizeof(*e));
        if (e == OSAL_NULL)
        {
            osal_mutex_unlock(p->lock);
            return OSAL_ENOMEM;
        }
        e->next = OSAL_NULL;
        e->key = es_strdup(key);
        e->value = es_strdup(value);
        e->quoted = 1u;

        if (e->key == OSAL_NULL || e->value == OSAL_NULL)
        {
            osal_free(e->key);
            osal_free(e->value);
            osal_free(e);
            osal_mutex_unlock(p->lock);
            return OSAL_ENOMEM;
        }

        /* 追加到尾部：落盘顺序与设置顺序一致，便于用 eeprom_read 排查 */
        if (p->head == OSAL_NULL)
        {
            p->head = e;
        }
        else
        {
            tail = p->head;
            while (tail->next != OSAL_NULL)
            {
                tail = tail->next;
            }
            tail->next = e;
        }
    }

    p->dirty = 1u;
    osal_mutex_unlock(p->lock);
    return OSAL_OK;
}

static char *es_get_value(eeprom_storage_t *es, const char *key)
{
    struct es_priv *p;
    struct es_entry *e;
    char *out;

    if (es == OSAL_NULL || es->priv == OSAL_NULL || key == OSAL_NULL)
    {
        return OSAL_NULL;
    }

    p = (struct es_priv *)es->priv;
    if (p->lock == OSAL_NULL)
    {
        return OSAL_NULL;
    }
    if (osal_mutex_lock(p->lock, OSAL_WAIT_FOREVER) != OSAL_OK)
    {
        return OSAL_NULL;
    }

    if (p->loaded == 0u && es_load_locked(p) != OSAL_OK)
    {
        osal_mutex_unlock(p->lock);
        return OSAL_NULL;
    }

    e = es_find(p->head, key);
    /* 在锁内复制一份再解锁：调用方拿到的是独立字符串，
     * 不会被后续 set_value 悄悄改掉 */
    out = (e != OSAL_NULL) ? es_strdup(e->value) : OSAL_NULL;

    osal_mutex_unlock(p->lock);
    return out;
}

static int es_load(eeprom_storage_t *es)
{
    struct es_priv *p;
    int ret;

    if (es == OSAL_NULL || es->priv == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }

    p = (struct es_priv *)es->priv;
    if (p->lock == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }
    if (osal_mutex_lock(p->lock, OSAL_WAIT_FOREVER) != OSAL_OK)
    {
        return OSAL_EBUSY;
    }

    ret = es_load_locked(p);

    osal_mutex_unlock(p->lock);
    return ret;
}

static int es_save(eeprom_storage_t *es)
{
    struct es_priv *p;
    int ret;

    if (es == OSAL_NULL || es->priv == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }

    p = (struct es_priv *)es->priv;
    if (p->lock == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }
    if (osal_mutex_lock(p->lock, OSAL_WAIT_FOREVER) != OSAL_OK)
    {
        return OSAL_EBUSY;
    }

    /* 没 load 过就存，等于用一份未知内容覆盖 ROM，先补读一次 */
    ret = (p->loaded != 0u) ? OSAL_OK : es_load_locked(p);
    if (ret == OSAL_OK)
    {
        ret = es_save_locked(p);
    }

    osal_mutex_unlock(p->lock);
    return ret;
}

/* ============================ 创建 / 销毁 ============================ */

/* 全系统共享的那一份配置（板级注册，msh 等模块取用） */
static eeprom_storage_t *s_default;

eeprom_storage_t *eeprom_storage_create(void)
{
    eeprom_storage_t *es;
    struct es_priv *p;

    es = (eeprom_storage_t *)osal_malloc(sizeof(*es));
    if (es == OSAL_NULL)
    {
        return OSAL_NULL;
    }

    p = (struct es_priv *)osal_malloc(sizeof(*p));
    if (p == OSAL_NULL)
    {
        osal_free(es);
        return OSAL_NULL;
    }

    memset(p, 0, sizeof(*p));
    p->lock = OSAL_NULL;            /* 互斥量在 init 时惰性创建 */

    es->init = es_init;
    es->set_value = es_set_value;
    es->get_value = es_get_value;
    es->load = es_load;
    es->save = es_save;
    es->priv = p;

    return es;
}

void eeprom_storage_destroy(eeprom_storage_t *es)
{
    struct es_priv *p;

    if (es == OSAL_NULL)
    {
        return;
    }

    if (s_default == es)
    {
        s_default = OSAL_NULL;      /* 别把默认实例留成野指针 */
    }

    p = (struct es_priv *)es->priv;
    if (p != OSAL_NULL)
    {
        es_list_free(p->head);
        if (p->lock != OSAL_NULL)
        {
            osal_mutex_delete(p->lock);
        }
        osal_free(p);
    }

    osal_free(es);
}

/* ============================ 默认实例 ============================ */

void eeprom_storage_set_default(eeprom_storage_t *es)
{
    s_default = es;
}

eeprom_storage_t *eeprom_storage_get_default(void)
{
    return s_default;
}
