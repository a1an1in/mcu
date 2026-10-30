/*
 * test_eeprom_storage.c - modules/storage 的 host 单元测试
 *
 * 把 eeprom_storage.c 原样编译进来，只把 i2c 层换成假的：
 *   fake_at24c02.c      假 I2C 总线 + 256 字节 RAM（可注坏、统计页写次数）
 *   fake_osal_storage.c malloc/free + 互斥量（顺便检查锁是否成对）
 *   eeprom.c + at24c02.c
 *                       真实的适配层与器件驱动（校验 / 分发 / 页切分 /
 *                       ACK 轮询 / ops / register），所以
 *                       「module → 适配层 → 器件」整条链路都真跑
 *
 * 覆盖的是「真机上很难验证错」的那些点：
 *   - ROM 布局（magic / 版本 / 长度 / CRC-8）与提交顺序；
 *   - 「没改动就不写」——用写次数断言，这是 EEPROM 寿命的关键；
 *   - 首次 set 会先自动 load，避免 save 把别的 key 抹掉；
 *   - 掉电后重新加载（新建实例 + load）能拿回原值；
 *   - 数据损坏（CRC 错 / 长度越界 / magic 不符 / JSON 语法错）分别是什么行为；
 *   - JSON 转义往返，以及「裸字面量」（数字 / true / null）原样写回不降级；
 *   - 容量边界（ES_ERR_NOSPACE）与非法参数；
 *   - 换一个非 AT24C02 的后端（纯 RAM ops）也能用 —— 证明新增器件只需加 ops。
 */
#include <stdio.h>
#include <string.h>

#include "at24c02.h"
#include "eeprom.h"
#include "eeprom_storage.h"

/* ---------------- fake_osal_storage.c 提供的观测接口 ---------------- */
int fake_osal_violations(void);
int fake_osal_lock_depth(void);

/* ---------------- 器件装配：真实适配层 + 假芯片 ----------------
 * module 只记设备指针、且要用很久，所以每次要新器件就给一个新槽位，
 * 不能复用同一个 struct eeprom_dev（否则改容量会影响还活着的实例）。 */
#define MAX_DEVS  32

static struct at24c02_dev s_ee[MAX_DEVS];
static struct eeprom_dev  s_rom[MAX_DEVS];
static int s_dev_used;

static struct eeprom_dev *make_rom(uint32_t capacity)
{
    struct eeprom_dev *rom;

    if (s_dev_used >= MAX_DEVS)
    {
        return NULL;
    }
    rom = &s_rom[s_dev_used];
    at24c02_init(&s_ee[s_dev_used], i2c_get_adapter(0), 0x50);

    /* 器件 → eeprom_ops 的注册在 at24c02.c 末尾，与真机是同一份代码 */
    if (at24c02_register(rom, &s_ee[s_dev_used], capacity) != EEPROM_OK)
    {
        return NULL;
    }
    s_dev_used++;
    return rom;
}

/* ---------------- 第二后端：一棵纯 RAM 的 EEPROM ----------------
 * 没有 I2C、没有页、没有写周期，只提供 ops 的两个函数 ——
 * 用来证明「新增一种存储介质」不需要动 eeprom.c / eeprom_storage.c。 */
#define RAM_ROM_SIZE 64u

static uint8_t s_ram_rom[RAM_ROM_SIZE];

static int ram_rom_read(struct eeprom_dev *dev, uint32_t addr, void *buf, size_t len)
{
    (void)dev;                  /* 一棵裸 RAM，不需要私有上下文 */
    memcpy(buf, &s_ram_rom[addr], len);
    return EEPROM_OK;
}

static int ram_rom_write(struct eeprom_dev *dev, uint32_t addr, const void *buf,
                         size_t len)
{
    (void)dev;
    memcpy(&s_ram_rom[addr], buf, len);
    return EEPROM_OK;
}

static const struct eeprom_ops s_ram_ops = {
    .read  = ram_rom_read,
    .write = ram_rom_write,
};

static struct eeprom_dev s_ram_dev =
    EEPROM_DEV_INITIALIZER(&s_ram_ops, NULL, RAM_ROM_SIZE);

/* module 的 init 校验用：假后端只为了「指针非空」，不会被调用 */
static int dummy_read(struct eeprom_dev *dev, uint32_t addr, void *buf, size_t len)
{
    (void)dev;
    (void)addr;
    (void)buf;
    (void)len;
    return EEPROM_OK;
}

static int dummy_write(struct eeprom_dev *dev, uint32_t addr, const void *buf,
                       size_t len)
{
    (void)dev;
    (void)addr;
    (void)buf;
    (void)len;
    return EEPROM_OK;
}

static const struct eeprom_ops s_dummy_ops = {
    .read  = dummy_read,
    .write = dummy_write,
};

static struct eeprom_dev s_cap5 =
    EEPROM_DEV_INITIALIZER(&s_dummy_ops, NULL, 5u);
static struct eeprom_dev s_cap_huge =
    EEPROM_DEV_INITIALIZER(&s_dummy_ops, NULL, 0x10005u);

/* ---------------- fake_at24c02.c 提供的观测接口 ---------------- */
void     fake_eeprom_reset(void);
uint8_t *fake_eeprom_raw(void);
void     fake_eeprom_poke(uint16_t addr, uint8_t val);
int      fake_eeprom_write_ops(void);           /* 页写次数 */
int      fake_eeprom_last_write_addr(void);     /* 最后一次页写的起始地址 */
void     fake_eeprom_set_present(int present);

/* ---------------- fake_osal_storage.c 提供的观测接口 ---------------- */
int fake_osal_violations(void);
int fake_osal_lock_depth(void);

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
        {                                                                 \
            printf("FAIL %d  %s\n", __LINE__, #cond);                     \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* 与实现同参数（0x07 / 初值 0 / 不反转），故意自己再写一份：
 * 测试如果直接调用被测代码算 CRC，就等于自己验证自己了 */
static uint8_t test_crc8(const uint8_t *data, size_t len)
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

/* 按模块的 ROM 布局，把一段正文塞进假 EEPROM（用来构造各种坏数据） */
static void program_rom(const char *text)
{
    uint8_t *rom = fake_eeprom_raw();
    size_t len = strlen(text);

    rom[0] = 0x45;                              /* magic 'E' */
    rom[1] = 0x01;                              /* 版本 1 */
    rom[2] = (uint8_t)(len & 0xFFu);
    rom[3] = (uint8_t)((len >> 8) & 0xFFu);
    rom[4] = test_crc8((const uint8_t *)text, len);
    memcpy(rom + 5, text, len);
}

/* 取正文（从偏移 5 起，长度取自头部），返回正文长度 */
static uint32_t rom_payload(char *out, size_t cap)
{
    const uint8_t *rom = fake_eeprom_raw();
    uint32_t len = (uint32_t)rom[2] | ((uint32_t)rom[3] << 8);

    if (len >= cap)
    {
        len = (uint32_t)cap - 1u;
    }
    memcpy(out, rom + 5, len);
    out[len] = '\0';
    return len;
}

/* want == NULL 表示期望「查不到」 */
static int expect_value(eeprom_storage_t *es, const char *key, const char *want)
{
    char *v = es->get_value(es, key);
    int ok;

    if (want == NULL)
    {
        ok = (v == NULL);
        if (!ok)
        {
            printf("FAIL  key=%s 期望不存在，实际 \"%s\"\n", key, v);
        }
    }
    else
    {
        ok = (v != NULL) && (strcmp(v, want) == 0);
        if (!ok)
        {
            printf("FAIL  key=%s 期望 \"%s\"，实际 %s\n",
                   key, want, (v != NULL) ? v : "(null)");
        }
    }

    osal_free(v);
    return ok;
}

/* 新建一个实例，模拟「掉电重启后重新读盘」 */
static eeprom_storage_t *fresh_instance(void)
{
    eeprom_storage_t *es = eeprom_storage_create();
    struct eeprom_dev *rom = make_rom(256u);

    if (es == NULL || rom == NULL)
    {
        eeprom_storage_destroy(es);
        return NULL;
    }
    if (es->init(es, rom) != OSAL_OK)
    {
        eeprom_storage_destroy(es);
        return NULL;
    }
    return es;
}

int main(void)
{
    eeprom_storage_t *es;
    char *v;
    char buf[128];
    int w0;

    /* ---- 1) 创建与参数校验 ---- */
    es = eeprom_storage_create();
    CHECK(es != NULL);
    CHECK(es->init != NULL && es->set_value != NULL && es->get_value != NULL &&
          es->load != NULL && es->save != NULL);

    {
        /* 没 init 之前一切都应该被挡住，而不是崩 */
        eeprom_storage_t *bare = eeprom_storage_create();

        CHECK(bare != NULL);
        CHECK(bare->get_value(bare, "a") == NULL);
        CHECK(bare->set_value(bare, "a", "1") == OSAL_EINVAL);
        CHECK(bare->load(bare) == OSAL_EINVAL);
        CHECK(bare->save(bare) == OSAL_EINVAL);
        eeprom_storage_destroy(bare);
    }

    fake_eeprom_reset();

    /* 适配层：器件绑定 + 核心层的参数 / 越界校验 */
    {
        struct eeprom_dev probe;
        struct at24c02_dev chip;
        uint8_t b[8] = { 0 };

        at24c02_init(&chip, i2c_get_adapter(0), 0x50);
        CHECK(at24c02_register(&probe, NULL, 256u) == EEPROM_ERR_PARAM);
        CHECK(at24c02_register(&probe, &chip, 0u) == EEPROM_ERR_PARAM);
        CHECK(at24c02_register(&probe, &chip, 512u) == EEPROM_ERR_PARAM);
        CHECK(at24c02_register(&probe, &chip, 256u) == EEPROM_OK);
        CHECK(eeprom_dev_valid(&probe) == 1);

        /* 核心层：空指针 / 越界 / 零长度，都不该碰器件 */
        CHECK(eeprom_read(NULL, 0u, b, 1u) == EEPROM_ERR_PARAM);
        CHECK(eeprom_read(&probe, 0u, NULL, 1u) == EEPROM_ERR_PARAM);
        CHECK(eeprom_write(&probe, 250u, b, 8u) == EEPROM_ERR_RANGE);
        CHECK(eeprom_read(&probe, 250u, b, 8u) == EEPROM_ERR_RANGE);
        CHECK(eeprom_read(&probe, 256u, b, 0u) == EEPROM_OK);   /* 尾部零长度 */
        CHECK(eeprom_write(&probe, 256u, b, 0u) == EEPROM_OK);
        CHECK(eeprom_read(&probe, 257u, b, 0u) == EEPROM_ERR_RANGE);
        CHECK(eeprom_write(&probe, 0u, b, 256u) == EEPROM_OK);  /* 正好整片 */
    }

    /* module：init 只校验设备本身 */
    CHECK(es->init(es, NULL) == OSAL_EINVAL);            /* 设备为空 */
    {
        struct eeprom_dev bad = { NULL, NULL, 0u };      /* ops 为空 */

        CHECK(es->init(es, &bad) == OSAL_EINVAL);
    }
    CHECK(es->init(es, &s_cap5) == OSAL_EINVAL);         /* 容量放不下头部 */
    CHECK(es->init(es, &s_cap_huge) == OSAL_EINVAL);     /* 16 位长度字段装不下 */
    CHECK(es->init(es, make_rom(256u)) == OSAL_OK);

    /* ---- 2) 全新片子（全 0xFF）：load 成功且为空，不报错 ---- */
    fake_eeprom_reset();
    CHECK(es->load(es) == OSAL_OK);
    CHECK(expect_value(es, "ssid", NULL));

    /* ---- 3) 基本 set / get ---- */
    CHECK(es->set_value(es, "ssid", "hello") == OSAL_OK);
    CHECK(expect_value(es, "ssid", "hello"));
    CHECK(expect_value(es, "nope", NULL));

    /* get 返回的是副本：改它不影响缓存 */
    v = es->get_value(es, "ssid");
    CHECK(v != NULL);
    if (v != NULL)
    {
        strcpy(v, "XXXX");
        osal_free(v);
    }
    CHECK(expect_value(es, "ssid", "hello"));

    /* ---- 4) save 的 ROM 布局：正文分页写 + 头部最后提交 ---- */
    w0 = fake_eeprom_write_ops();
    CHECK(es->save(es) == OSAL_OK);
    CHECK(fake_eeprom_write_ops() > w0);                /* 确实写了（正文分页写） */
    /* 头部固定落在 0x00，而且是最后一步的「提交点」：
     * 正文写到一半掉电时头部还是旧的，load 会判定损坏而不是读出半截数据 */
    CHECK(fake_eeprom_last_write_addr() == 0);
    CHECK(rom_payload(buf, sizeof(buf)) == strlen("{\"ssid\":\"hello\"}"));
    CHECK(strcmp(buf, "{\"ssid\":\"hello\"}") == 0);

    {
        const uint8_t *rom = fake_eeprom_raw();
        uint32_t len = (uint32_t)rom[2] | ((uint32_t)rom[3] << 8);

        CHECK(rom[0] == 0x45);
        CHECK(rom[1] == 0x01);
        CHECK(rom[4] == test_crc8(rom + 5, len));
    }

    /* ---- 5) 没改动时 save 一个字节都不写（EEPROM 寿命） ---- */
    w0 = fake_eeprom_write_ops();
    CHECK(es->save(es) == OSAL_OK);
    CHECK(fake_eeprom_write_ops() == w0);

    /* ---- 6) 设成同样的值也不算改动 ---- */
    CHECK(es->set_value(es, "ssid", "hello") == OSAL_OK);
    CHECK(es->save(es) == OSAL_OK);
    CHECK(fake_eeprom_write_ops() == w0);

    /* ---- 7) 模拟掉电重启：新建实例 + load 能拿回原值 ---- */
    {
        eeprom_storage_t *es2 = fresh_instance();

        CHECK(es2 != NULL);
        CHECK(es2->load(es2) == OSAL_OK);
        CHECK(expect_value(es2, "ssid", "hello"));
        eeprom_storage_destroy(es2);
    }

    /* ---- 8) 多项 + 覆盖已有 key（原地改值，不新增一项） ---- */
    CHECK(es->set_value(es, "port", "8080") == OSAL_OK);
    CHECK(es->set_value(es, "name", "mcu") == OSAL_OK);
    CHECK(es->set_value(es, "port", "9090") == OSAL_OK);
    CHECK(es->save(es) == OSAL_OK);
    rom_payload(buf, sizeof(buf));
    CHECK(strcmp(buf, "{\"ssid\":\"hello\",\"port\":\"9090\",\"name\":\"mcu\"}") == 0);
    CHECK(expect_value(es, "port", "9090"));

    /* ---- 9) 转义往返：值里的引号 / 换行 / 制表 / 反斜杠，键里的引号 ---- */
    {
        static const char WANT[] = "he said \"hi\"\n\t\\";   /* 12 ~ 20 字节，含特殊字符 */
        static const char KEY[]  = "q\"k\\y";
        eeprom_storage_t *es3;

        CHECK(es->set_value(es, KEY, WANT) == OSAL_OK);
        CHECK(es->save(es) == OSAL_OK);

        es3 = fresh_instance();
        CHECK(es3 != NULL);
        CHECK(es3->load(es3) == OSAL_OK);
        CHECK(expect_value(es3, KEY, WANT));
        eeprom_storage_destroy(es3);
    }

    /* ---- 10) 新实例没显式 load 就 set，save 不能把老 key 抹掉 ---- */
    {
        eeprom_storage_t *es4 = fresh_instance();

        CHECK(es4 != NULL);
        CHECK(es4->set_value(es4, "extra", "1") == OSAL_OK);   /* 内部会先 load */
        CHECK(es4->save(es4) == OSAL_OK);
        eeprom_storage_destroy(es4);

        CHECK(es->load(es) == OSAL_OK);
        CHECK(expect_value(es, "extra", "1"));
        CHECK(expect_value(es, "ssid", "hello"));              /* 老 key 还在 */
        CHECK(expect_value(es, "port", "9090"));
    }

    /* ---- 11) 容量边界：正文上限 = size - 5 ---- */
    fake_eeprom_reset();
    {
        eeprom_storage_t *es5 = eeprom_storage_create();

        CHECK(es5 != NULL);
        CHECK(es5->init(es5, make_rom(16u)) == OSAL_OK);       /* 正文只有 11 字节 */
        /* {"k":"vvvv"} = 12 字节 > 11 → 放不下 */
        CHECK(es5->set_value(es5, "k", "vvvv") == ES_ERR_NOSPACE);
        /* {"k":"vvv"} = 11 字节 → 正好填满 */
        CHECK(es5->set_value(es5, "k", "vvv") == OSAL_OK);
        CHECK(es5->save(es5) == OSAL_OK);
        CHECK(expect_value(es5, "k", "vvv"));
        eeprom_storage_destroy(es5);
    }

    /* ---- 12) 坏数据：每种都必须是「报错 + 空缓存」，不能读出错值 ---- */
    {
        eeprom_storage_t *es6 = eeprom_storage_create();

        CHECK(es6 != NULL);
        CHECK(es6->init(es6, make_rom(256u)) == OSAL_OK);

        /* 12.1 CRC 不符 */
        fake_eeprom_reset();
        program_rom("{\"a\":\"1\"}");
        fake_eeprom_poke(6, 'b');               /* 改正文 → CRC 对不上 */
        CHECK(es6->load(es6) == ES_ERR_FORMAT);
        CHECK(expect_value(es6, "a", NULL));
        CHECK(expect_value(es6, "b", NULL));

        /* 12.2 magic 不符 → 当成出厂空片，load 成功且为空 */
        fake_eeprom_reset();
        program_rom("{\"a\":\"1\"}");
        fake_eeprom_poke(0, 0x00);
        CHECK(es6->load(es6) == OSAL_OK);
        CHECK(expect_value(es6, "a", NULL));

        /* 12.3 头部长度越界 */
        fake_eeprom_reset();
        program_rom("{\"a\":\"1\"}");
        fake_eeprom_raw()[2] = 0xFFu;
        fake_eeprom_raw()[3] = 0x00u;
        CHECK(es6->load(es6) == ES_ERR_FORMAT);

        /* 12.4 不是对象 */
        fake_eeprom_reset();
        program_rom("[1,2,3]");
        CHECK(es6->load(es6) == ES_ERR_FORMAT);

        /* 12.5 缺冒号 */
        fake_eeprom_reset();
        program_rom("{\"a\" 1}");
        CHECK(es6->load(es6) == ES_ERR_FORMAT);

        /* 12.6 嵌套对象 */
        fake_eeprom_reset();
        program_rom("{\"a\":{\"b\":1}}");
        CHECK(es6->load(es6) == ES_ERR_FORMAT);

        /* 12.7 嵌套数组 */
        fake_eeprom_reset();
        program_rom("{\"a\":[1,2]}");
        CHECK(es6->load(es6) == ES_ERR_FORMAT);

        /* 12.8 非法转义 */
        fake_eeprom_reset();
        program_rom("{\"a\":\"x\\q\"}");
        CHECK(es6->load(es6) == ES_ERR_FORMAT);

        /* 12.9 对象后面还有垃圾 */
        fake_eeprom_reset();
        program_rom("{\"a\":\"1\"} x");
        CHECK(es6->load(es6) == ES_ERR_FORMAT);

        /* 12.10 器件不响应 → IO 错误；恢复后还能正常读到 */
        fake_eeprom_reset();
        program_rom("{\"a\":\"1\"}");
        fake_eeprom_set_present(0);
        CHECK(es6->load(es6) == OSAL_EIO);
        fake_eeprom_set_present(1);
        CHECK(es6->load(es6) == OSAL_OK);
        CHECK(expect_value(es6, "a", "1"));

        /* 12.11 空行 / 多余空白是合法的 JSON */
        fake_eeprom_reset();
        program_rom("{ \"a\" : \"1\" ,\n\r\t \"b\" : \"2\" }");
        CHECK(es6->load(es6) == OSAL_OK);
        CHECK(expect_value(es6, "a", "1"));
        CHECK(expect_value(es6, "b", "2"));

        /* 12.12 重复键：以最后一次为准 */
        fake_eeprom_reset();
        program_rom("{\"a\":\"1\",\"a\":\"2\"}");
        CHECK(es6->load(es6) == OSAL_OK);
        CHECK(expect_value(es6, "a", "2"));

        eeprom_storage_destroy(es6);
    }

    /* ---- 13) 裸字面量：数字 / true / null 原样保留，不会被降级成字符串 ---- */
    fake_eeprom_reset();
    program_rom("{\"port\":8080,\"on\":true,\"nil\":null,\"name\":\"x\"}");
    {
        eeprom_storage_t *es7 = fresh_instance();

        CHECK(es7 != NULL);
        CHECK(es7->load(es7) == OSAL_OK);
        /* 对外一律是字符串 */
        CHECK(expect_value(es7, "port", "8080"));
        CHECK(expect_value(es7, "on", "true"));
        CHECK(expect_value(es7, "nil", "null"));

        /* 没改动 → 一个字节都不写 */
        w0 = fake_eeprom_write_ops();
        CHECK(es7->save(es7) == OSAL_OK);
        CHECK(fake_eeprom_write_ops() == w0);

        /* 改其它 key → 裸字面量必须原样写回（8080 不能变成 "8080"） */
        CHECK(es7->set_value(es7, "name", "y") == OSAL_OK);
        CHECK(es7->save(es7) == OSAL_OK);
        rom_payload(buf, sizeof(buf));
        CHECK(strcmp(buf, "{\"port\":8080,\"on\":true,\"nil\":null,\"name\":\"y\"}") == 0);

        /* 通过 API 覆盖写之后，这一项就变成字符串了 */
        CHECK(es7->set_value(es7, "port", "9090") == OSAL_OK);
        CHECK(es7->save(es7) == OSAL_OK);
        rom_payload(buf, sizeof(buf));
        CHECK(strcmp(buf, "{\"port\":\"9090\",\"on\":true,\"nil\":null,\"name\":\"y\"}") == 0);

        eeprom_storage_destroy(es7);
    }

    /* ---- 14) \uXXXX：BMP 内的码点转成 UTF-8；\u0000 拒绝 ---- */
    fake_eeprom_reset();
    program_rom("{\"c\":\"\\u4e2d\\u6587\",\"e\":\"\\u00e9\"}");
    {
        eeprom_storage_t *es8 = fresh_instance();

        CHECK(es8 != NULL);
        CHECK(es8->load(es8) == OSAL_OK);
        CHECK(expect_value(es8, "c", "中文"));
        CHECK(expect_value(es8, "e", "é"));
        eeprom_storage_destroy(es8);
    }
    fake_eeprom_reset();
    program_rom("{\"nul\":\"\\u0000\"}");
    {
        eeprom_storage_t *es9 = fresh_instance();

        CHECK(es9 != NULL);
        CHECK(es9->load(es9) == ES_ERR_FORMAT);     /* C 字符串装不下 NUL */
        eeprom_storage_destroy(es9);
    }

    /* ---- 15) 参数校验：只有空 key / NULL 是非法的，其余字符都能存 ---- */
    CHECK(es->set_value(es, "", "x") == OSAL_EINVAL);
    CHECK(es->set_value(es, NULL, "x") == OSAL_EINVAL);
    CHECK(es->set_value(es, "k", NULL) == OSAL_EINVAL);

    fake_eeprom_reset();
    CHECK(es->load(es) == OSAL_OK);
    CHECK(es->set_value(es, "a=b", "x") == OSAL_OK);        /* key 里有 '=' 也 OK */
    CHECK(es->set_value(es, "k", "x\ny") == OSAL_OK);       /* 值里有换行也 OK */
    CHECK(es->save(es) == OSAL_OK);
    rom_payload(buf, sizeof(buf));
    CHECK(strcmp(buf, "{\"a=b\":\"x\",\"k\":\"x\\ny\"}") == 0);
    CHECK(expect_value(es, "a=b", "x"));
    CHECK(expect_value(es, "k", "x\ny"));

    /* ---- 16) 空值可存，也就是「置空」的写法 ---- */
    fake_eeprom_reset();
    CHECK(es->load(es) == OSAL_OK);
    CHECK(es->set_value(es, "tmp", "v") == OSAL_OK);
    CHECK(es->save(es) == OSAL_OK);
    CHECK(es->set_value(es, "tmp", "") == OSAL_OK);
    CHECK(es->save(es) == OSAL_OK);
    rom_payload(buf, sizeof(buf));
    CHECK(strcmp(buf, "{\"tmp\":\"\"}") == 0);
    CHECK(expect_value(es, "tmp", ""));

    /* ---- 17) 换后端：一棵没有 I2C、没有页的 RAM，只提供 ops ----
     * 这就是「新增器件要改多少」的答案：一个 ops 表，其它什么都不用动。 */
    {
        eeprom_storage_t *r1 = eeprom_storage_create();
        eeprom_storage_t *r2 = eeprom_storage_create();

        memset(s_ram_rom, 0xFF, sizeof(s_ram_rom));     /* 全新「片子」 */
        CHECK(r1 != NULL && r2 != NULL);
        CHECK(r1->init(r1, &s_ram_dev) == OSAL_OK);
        CHECK(r1->set_value(r1, "fmt", "ram") == OSAL_OK);
        CHECK(r1->save(r1) == OSAL_OK);
        eeprom_storage_destroy(r1);

        /* 新实例从同一个后端读回，等价于「掉电重启」 */
        CHECK(r2->init(r2, &s_ram_dev) == OSAL_OK);
        CHECK(r2->load(r2) == OSAL_OK);
        CHECK(expect_value(r2, "fmt", "ram"));
        eeprom_storage_destroy(r2);
    }

    /* ---- 18) 锁：所有接口都要成对加解锁，且不能重入 ---- */
    CHECK(fake_osal_violations() == 0);
    CHECK(fake_osal_lock_depth() == 0);

    eeprom_storage_destroy(es);
    eeprom_storage_destroy(NULL);           /* 传 NULL 必须安全 */

    if (failures == 0)
    {
        printf("PASS: eeprom_storage 全部断言通过"
               "（布局/CRC、脏标记、自动 load、坏数据、JSON 转义与裸字面量、"
               "容量与参数、换 ops 后端）\n");
        return 0;
    }

    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
