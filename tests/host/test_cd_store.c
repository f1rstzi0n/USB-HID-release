#include "bvpv_cd_store.h"
#include "bvpv_catalog_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint8_t flash[0x800000], baseline[0x800000], factory[4096], update[6144];
static bvpv_cd_store_t store;
static unsigned operations, cut_at, cut_bytes, busy, polls, reads;
static bool dead, reserved, read_fail, poll_fail, deny_reserve, corrupt_read;
static uint32_t ticks;
static bool checking_boot;

static bvpv_cd_read_result_t read_io(void *ctx, uint32_t a, uint8_t *p, uint32_t n)
{
    (void)ctx;
    CHECK(!busy);
    CHECK(a < sizeof(flash) && n <= sizeof(flash) - a);
    reads++;
    if (checking_boot) CHECK(n <= 256u);
    if (dead || read_fail)
        return BVPV_CD_READ_ERROR;
    memcpy(p, flash + a, n);
    if (corrupt_read) { p[0] ^= 1; corrupt_read = false; }
    return BVPV_CD_READ_OK;
}
static bool erase_io(void *ctx, uint32_t a)
{
    unsigned n = 4096;
    (void)ctx;
    CHECK(reserved && !busy);
    CHECK(a % 4096 == 0 && a <= sizeof(flash) - 4096);
    CHECK(a < BVPV_CD_IMAGE_AREA_BYTES ||
          (a >= BVPV_CD_META_BASE && a < BVPV_CD_META_BASE + 8192u));
    operations++;
    if (dead)
        return false;
    if (operations == cut_at)
    {
        n = cut_bytes < 4096 ? cut_bytes : 4096;
        dead = true;
    }
    memset(flash + a, 255, n);
    busy = dead ? 0 : 2;
    return !dead;
}
static bool program_io(void *ctx, uint32_t a, const uint8_t *p, uint16_t n)
{
    unsigned i;
    (void)ctx;
    CHECK(reserved && !busy);
    CHECK(n && n <= 256 && a % 256 + n <= 256);
    CHECK(a < sizeof(flash) && n <= sizeof(flash) - a);
    CHECK((a < BVPV_CD_IMAGE_AREA_BYTES && n <= BVPV_CD_IMAGE_AREA_BYTES - a) ||
          (a >= BVPV_CD_META_BASE && a < BVPV_CD_META_BASE + 8192u &&
           n <= BVPV_CD_META_BASE + 8192u - a));
    operations++;
    if (dead)
        return false;
    if (operations == cut_at)
    {
        if (cut_bytes < n)
            n = (uint16_t)cut_bytes;
        dead = true;
    }
    for (i = 0; i < n; i++)
        flash[a + i] &= p[i];
    busy = dead ? 0 : 2;
    return !dead;
}
static int poll_io(void *ctx)
{
    (void)ctx;
    polls++;
    if (poll_fail)
    {
        busy = 0;
        return -1;
    }
    if (busy)
    {
        busy--;
        return 1;
    }
    return dead ? -1 : 0;
}
static bool reserve_io(void *ctx, bool lock)
{
    (void)ctx;
    if (lock && deny_reserve)
        return false;
    reserved = lock;
    return true;
}
static const bvpv_cd_io_t io = {0, read_io, erase_io, program_io, poll_io, reserve_io};
static void reset_io(void)
{
    busy = 0;
    dead = false;
    reserved = false;
    operations = 0;
    cut_at = 0;
    ticks = 0;
    read_fail = false;
    poll_fail = false;
    deny_reserve = false;
    corrupt_read = false;
}
static void boot(void)
{
    reset_io();
    bvpv_cd_store_init(&store, &io);
    CHECK(!bvpv_cd_store_boot(&store));
    CHECK(store.booting && !store.available);
    checking_boot = true;
    while (store.booting) bvpv_cd_store_task(&store, ++ticks);
    checking_boot = false;
    CHECK(operations == 0);
    CHECK(!reserved);
}
static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}
static void put32(uint8_t *p, uint32_t n)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (i * 8));
}
static uint8_t request(uint8_t svc, const uint8_t *p, uint16_t len)
{
    uint8_t response[27];
    uint16_t n;
    unsigned i;
    bvpv_cd_store_request(&store, svc, p, len, ++ticks);
    for (i = 0; i < 100000; i++)
    {
        if (bvpv_cd_store_take_reply(&store, response, &n))
        {
            CHECK(n > 0);
            if (svc == 15 && response[0] == 0)
            {
                uint8_t expected[25] = {0};
                expected[1] = BVPV_CD_LAYOUT_VERSION;
                expected[2] = (uint8_t)bvpv_cd_store_state(&store);
                expected[3] = store.available ? 1 : 0;
                expected[4] = store.last_error;
                put32(expected + 5, BVPV_CD_SLOT_BYTES);
                put32(expected + 9, store.available ? store.active.bytes : 0);
                put32(expected + 13, store.available ? store.active.crc32 : 0);
                put32(expected + 17, store.high_sequence);
                put32(expected + 21, store.offset);
                CHECK(n == sizeof(expected) && !memcmp(response, expected, n));
            }
            return response[0];
        }
        bvpv_cd_store_task(&store, ++ticks);
    }
    CHECK(0);
    return 255;
}
static uint8_t begin(uint32_t bytes, uint32_t crc)
{
    uint8_t p[8];
    put32(p, bytes);
    put32(p + 4, crc);
    return request(10, p, sizeof(p));
}
static uint8_t block(uint32_t off)
{
    uint8_t p[2054];
    put32(p, off);
    put16(p + 4, 2048);
    memcpy(p + 6, update + off, 2048);
    return request(11, p, sizeof(p));
}
static uint8_t transfer(void)
{
    uint32_t off;
    uint8_t status = begin(sizeof(update), bvpv_cd_crc32(update, sizeof(update)));
    if (status)
        return status;
    for (off = 0; off < sizeof(update); off += 2048)
    {
        status = block(off);
        if (status)
            return status;
    }
    return request(12, 0, 0);
}
static void assert_contents(const uint8_t *p, uint32_t n)
{
    uint8_t buf[256];
    uint32_t off;
    CHECK(store.active.bytes == n);
    for (off = 0; off < n; off += sizeof(buf))
    {
        CHECK(bvpv_cd_store_read(&store, off, buf, sizeof(buf)) == BVPV_CD_READ_OK);
        CHECK(!memcmp(buf, p + off, sizeof(buf)));
    }
}
static void assert_reserved_unchanged(void)
{
    CHECK(!memcmp(flash + BVPV_FLASH_PARAM_BASE, baseline + BVPV_FLASH_PARAM_BASE,
                  BVPV_CD_META_BASE - BVPV_FLASH_PARAM_BASE));
    CHECK(!memcmp(flash + BVPV_CD_META_BASE + 8192, baseline + BVPV_CD_META_BASE + 8192,
                  BVPV_CD_META_AREA_BYTES - 8192));
}
static void seed_record(unsigned index, const bvpv_cd_record_t *r)
{
    uint8_t encoded[32];
    bvpv_cd_record_encode(encoded, r);
    encoded[28] = BVPV_CD_META_VALID;
    memcpy(flash + BVPV_CD_RECORD_BASE(index), encoded, sizeof(encoded));
}
static void assert_empty(void)
{
    uint8_t byte;
    CHECK(!store.available && !store.fault && !bvpv_cd_store_ready(&store));
    CHECK(bvpv_cd_store_read(&store, 0, &byte, 1) == BVPV_CD_READ_ERROR);
    CHECK(request(15, 0, 0) == 0);
}
static void test_update_and_errors(void)
{
    uint32_t crc = bvpv_cd_crc32(update, sizeof(update));
    uint8_t bad[11] = {0};
    memcpy(flash, baseline, sizeof(flash)); boot();
    CHECK(begin(0, crc) == 3);
    CHECK(begin(BVPV_CD_SLOT_BYTES + 2048, crc) == 3);
    CHECK(begin(2049, crc) == 8);
    CHECK(request(10, bad, sizeof(bad)) == 8);
    CHECK(operations == 0 && !reserved);
    assert_contents(factory, sizeof(factory));
    deny_reserve = true; CHECK(begin(2048, crc) == 6); deny_reserve = false;
    CHECK(begin(sizeof(update), crc) == 0);
    CHECK(reserved && !store.available && !store.committed && operations == 3);
    CHECK(store.active.kind == BVPV_CD_RECORD_NO_MEDIA && store.high_sequence == 2);
    CHECK(block(2048) == 11 && block(0) == 0 && block(0) == 11);
    CHECK(request(12, 0, 0) == 11);
    CHECK(request(13, 0, 0) == 0); assert_empty();
    boot(); assert_empty(); CHECK(store.high_sequence == 2);
    CHECK(transfer() == 0 && store.active.sequence == 4);
    assert_contents(update, sizeof(update)); boot(); assert_contents(update, sizeof(update));
    CHECK(transfer() == 0 && store.active.sequence == 6); /* 相同镜像也必须前进序号。 */
    CHECK(begin(sizeof(update), 1) == 0);
    CHECK(block(0) == 0 && block(2048) == 0 && block(4096) == 0);
    CHECK(request(12, 0, 0) == 7); assert_empty(); boot(); assert_empty();
    CHECK(store.high_sequence == 7 && transfer() == 0 && store.active.sequence == 9);
    CHECK(request(15, 0, 1) == 8);
    assert_reserved_unchanged();
    memcpy(flash, baseline, sizeof(flash)); boot();
    corrupt_read = true; CHECK(begin(sizeof(update), crc) == 7);
    CHECK(!reserved); boot(); assert_contents(factory, sizeof(factory));
    CHECK(begin(sizeof(update), crc) == 0);
    corrupt_read = true; CHECK(block(0) == 7);
    CHECK(!reserved); boot(); assert_empty();
    CHECK(transfer() == 0); assert_reserved_unchanged();
}
/* 每个擦写边界切断电源；重启禁止隐式修复，空盘只能显式整包重刷。 */
static void test_power_cuts(void)
{
    unsigned op, n, total, old_count = 0, empty_count = 0, new_count = 0;
    uint8_t *snapshot = malloc(sizeof(flash));
    CHECK(snapshot);
    memcpy(flash, baseline, sizeof(flash)); boot();
    memcpy(snapshot, flash, sizeof(flash));
    CHECK(transfer() == 0); total = operations;
    for (op = 1; op <= total; op++)
        for (n = 0; n < 39; n++)
        {
            static const unsigned tail[] = {127, 255, 256, 2048, 4095, 4096};
            memcpy(flash, snapshot, sizeof(flash)); boot();
            cut_at = op; cut_bytes = n <= 32 ? n : tail[n - 33];
            (void)transfer(); boot();
            if (!store.available)
            {
                CHECK(store.high_sequence == 2);
                assert_empty(); empty_count++;
            }
            else if (store.high_sequence == 1)
            {
                assert_contents(factory, sizeof(factory)); old_count++;
            }
            else
            {
                CHECK(store.high_sequence == 3);
                assert_contents(update, sizeof(update)); new_count++;
            }
            assert_reserved_unchanged();
            CHECK(transfer() == 0); boot(); assert_contents(update, sizeof(update));
            assert_reserved_unchanged();
        }
    CHECK(old_count && empty_count && new_count);
    printf("power cuts: %u boundaries x 39 writes; old=%u empty=%u new=%u; reflash passed\n",
           total, old_count, empty_count, new_count);
    free(snapshot);
}
static void test_cancel_busy_and_io_fault(void)
{
    uint8_t p[2054], begin_payload[8]; unsigned i;
    memcpy(flash, baseline, sizeof(flash)); boot();
    put32(begin_payload, sizeof(update)); put32(begin_payload + 4, bvpv_cd_crc32(update, sizeof(update)));
    bvpv_cd_store_request(&store, 10, begin_payload, sizeof(begin_payload), ++ticks);
    CHECK(store.preparing_begin && !store.reply_ready && operations == 0);
    bvpv_cd_store_cancel(&store); bvpv_cd_store_task(&store, ++ticks);
    CHECK(!reserved); assert_contents(factory, sizeof(factory));
    memcpy(flash, baseline, sizeof(flash)); boot();
    bvpv_cd_store_request(&store, 10, begin_payload, sizeof(begin_payload), ++ticks);
    for (i = 0; i < 50 && !(busy && store.meta[28] == BVPV_CD_META_VALID); i++)
        bvpv_cd_store_task(&store, ++ticks);
    CHECK(busy && store.preparing_begin && !store.reply_ready && !store.committed);
    bvpv_cd_store_cancel(&store); bvpv_cd_store_task(&store, ++ticks); CHECK(reserved);
    for (i = 0; i < 10 && reserved; i++) bvpv_cd_store_task(&store, ++ticks);
    CHECK(!reserved && !store.committed && store.high_sequence == 2);
    boot(); assert_empty(); CHECK(store.high_sequence == 2 && transfer() == 0);
    CHECK(store.high_sequence == 4);
    CHECK(begin(sizeof(update), bvpv_cd_crc32(update, sizeof(update))) == 0);
    put32(p, 0); put16(p + 4, 2048); memcpy(p + 6, update, 2048);
    bvpv_cd_store_request(&store, 11, p, sizeof(p), ++ticks); CHECK(busy);
    bvpv_cd_store_cancel(&store); bvpv_cd_store_task(&store, ++ticks); CHECK(reserved);
    for (i = 0; i < 5; i++) bvpv_cd_store_task(&store, ++ticks);
    CHECK(!reserved); assert_empty(); boot(); assert_empty();
    CHECK(begin(sizeof(update), 0) == 0);
    bvpv_cd_store_task(&store, ticks + BVPV_CD_IDLE_TIMEOUT_MS);
    CHECK(!reserved); assert_empty();
    CHECK(begin(sizeof(update), 0) == 0);
    poll_fail = true; CHECK(block(0) == 255); CHECK(store.fault && !reserved);
    boot(); assert_empty(); CHECK(transfer() == 0);
    read_fail = true; bvpv_cd_store_init(&store, &io); operations = 0;
    CHECK(!bvpv_cd_store_boot(&store));
    while (store.booting) bvpv_cd_store_task(&store, ++ticks);
    CHECK(store.fault && operations == 0 && !reserved);
    CHECK(begin(sizeof(update), 0) == 10);
    assert_reserved_unchanged();
}
static void test_full_capacity_and_wrap(void)
{
    uint8_t payload[2054]; uint32_t crc = 0, off; bvpv_cd_record_t r;
    memcpy(flash, baseline, sizeof(flash)); boot();
    r = store.active; r.sequence = 0xFFFFFFFFu; seed_record(0, &r); boot();
    CHECK(store.high_sequence == 0xFFFFFFFFu);
    memset(payload + 6, 0x6B, 2048); put16(payload + 4, 2048);
    for (off = 0; off < BVPV_CD_SLOT_BYTES; off += 2048)
        crc = bvpv_cd_crc32_update(crc, payload + 6, 2048);
    CHECK(begin(BVPV_CD_SLOT_BYTES, crc) == 0 && store.high_sequence == 0);
    for (off = 0; off < BVPV_CD_SLOT_BYTES; off += 2048)
    { put32(payload, off); CHECK(request(11, payload, sizeof(payload)) == 0); }
    CHECK(request(12, 0, 0) == 0 && store.high_sequence == 1);
    boot(); CHECK(store.available && store.active.bytes == BVPV_CD_SLOT_BYTES && store.high_sequence == 1);
    CHECK(flash[BVPV_CD_SLOT_BYTES - 1] == 0x6B);
    assert_reserved_unchanged();
}
static void test_blank_legacy_and_corrupt(void)
{
    unsigned i;
    bvpv_cd_record_t r;
    const char *legacy[] = {"BVC3", "BVC2", "BVCD"};
    memset(flash, 255, sizeof(flash)); boot(); assert_empty();
    CHECK(store.high_sequence == 0 && transfer() == 0 && store.high_sequence == 2);
    for (i = 0; i < 3; i++)
    {
        memcpy(flash, baseline, sizeof(flash));
        memset(flash + BVPV_CD_META_BASE, 255, 8192);
        memcpy(flash, legacy[i], 4);
        boot(); CHECK(store.last_error == 9); assert_empty();
        CHECK(!memcmp(flash, legacy[i], 4) && transfer() == 0);
    }
    memcpy(flash, baseline, sizeof(flash)); boot(); CHECK(transfer() == 0);
    flash[10] ^= 1; boot(); CHECK(store.last_error == 7); assert_empty();
    CHECK(store.high_sequence == 3 && transfer() == 0 && store.high_sequence == 5);
    flash[BVPV_CD_RECORD_BASE(store.record_index) + 24] ^= 1;
    boot(); assert_empty(); /* 另一份为墓碑，损坏新记录不恢复旧镜像。 */
    CHECK(transfer() == 0); assert_reserved_unchanged();
    memcpy(flash, baseline, sizeof(flash)); boot();
    r = store.active; seed_record(1, &r); boot();
    assert_contents(factory, sizeof(factory)); /* 完全一致的重复记录可挂载。 */
    r.kind = BVPV_CD_RECORD_NO_MEDIA; r.bytes = r.crc32 = 0;
    seed_record(1, &r); boot(); CHECK(store.last_error == 7); assert_empty();
    CHECK(store.high_sequence == 1 && transfer() == 0 && store.high_sequence == 3);
    memcpy(flash, baseline, sizeof(flash)); boot();
    r = store.active; r.sequence += 0x80000000u; seed_record(1, &r);
    boot(); CHECK(store.last_error == 7); assert_empty();
    CHECK(transfer() == 0); boot(); assert_contents(update, sizeof(update));
    assert_reserved_unchanged();
}
static void seed_signed_catalog(void)
{
    uint8_t package[8192],record[96];char path[512];FILE *f;size_t n;
    CHECK(snprintf(path,sizeof(path),"%s/valid.acat",CATALOG_FIXTURE_DIR)>0);
    f=fopen(path,"rb");CHECK(f);n=fread(package,1,sizeof(package),f);CHECK(n>512u && !ferror(f) && fgetc(f)==EOF);fclose(f);
    memcpy(flash+BVPV_FLASH_PARAM_BASE,package,n);
    memset(flash+BVPV_CATALOG_META_BASE(0),255,8192);
    memset(record,255,sizeof(record));memcpy(record,"ACC1",4);record[4]=0;memset(record+5,0,3);
    put32(record+8,2);put32(record+12,(uint32_t)n);memcpy(record+16,package+64,32);memcpy(record+48,package+32,32);
    put32(record+80,bvpv_cd_crc32(record,80));record[92]=0xA5;
    memcpy(flash+BVPV_CATALOG_META_BASE(0),record,sizeof(record));
}
int main(void)
{
    unsigned i; bvpv_cd_record_t r = {BVPV_CD_RECORD_IMAGE, sizeof(factory), 0, 1, 1};
    for (i = 0; i < sizeof(factory); i++) factory[i] = (uint8_t)(i * 7u);
    for (i = 0; i < sizeof(update); i++) update[i] = (uint8_t)(i * 11u + 3u);
    memset(flash, 255, sizeof(flash));
    memset(flash + BVPV_FLASH_PARAM_BASE, 0x35, BVPV_CD_META_BASE - BVPV_FLASH_PARAM_BASE);
    memset(flash + BVPV_CD_META_BASE + 8192, 0x76, BVPV_CD_META_AREA_BYTES - 8192);
    seed_signed_catalog();
    memcpy(flash, factory, sizeof(factory)); r.crc32 = bvpv_cd_crc32(factory, sizeof(factory));
    seed_record(0, &r); boot(); assert_contents(factory, sizeof(factory));
    memcpy(baseline, flash, sizeof(flash));
    test_update_and_errors(); test_power_cuts(); test_cancel_busy_and_io_fault();
    test_full_capacity_and_wrap(); test_blank_legacy_and_corrupt();
    puts("cd_store passed: single image, tombstones, partition protection and recovery");
    return 0;
}
