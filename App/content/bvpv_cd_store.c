#include "bvpv_cd_store.h"
#include <string.h>

enum
{
    PH_NONE,
    PH_ERASE_WAIT,
    PH_PAGE_START,
    PH_PAGE_WAIT,
    PH_BLOCK_CHECK,
    PH_VERIFY,
    PH_META_ERASE,
    PH_META_ERASE_WAIT,
    PH_META_BODY,
    PH_META_BODY_WAIT,
    PH_META_CHECK,
    PH_MARK_WAIT
};
enum
{
    OK = 0,
    RANGE = 3,
    BUSY = 6,
    CRC = 7,
    LENGTH = 8,
    UNSUPPORTED = 9,
    STATE = 10,
    ORDER = 11,
    IO_ERROR = 255
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}
static void wr32(uint8_t *p, uint32_t n)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (8 * i));
}

void bvpv_cd_store_init(bvpv_cd_store_t *s, const bvpv_cd_io_t *io)
{
    memset(s, 0, sizeof(*s));
    if (io) s->io = *io;
}
bool bvpv_cd_store_boot(bvpv_cd_store_t *s)
{
    s->available = false;
    s->booting = true;
    s->boot_phase = 0;
    s->cursor = s->crc = 0;
    return false;
}
static void boot_task(bvpv_cd_store_t *s)
{
    bvpv_cd_record_t records[2];
    bool valid[2];
    uint32_t addr, n;
    bvpv_cd_read_result_t result;
    if (s->boot_phase < 2u)
    {
        result = s->io.read(s->io.ctx, BVPV_CD_RECORD_BASE(s->boot_phase),
                            s->boot_raw[s->boot_phase], BVPV_CD_RECORD_LEN);
        if (result == BVPV_CD_READ_BUSY) return;
        if (result != BVPV_CD_READ_OK) goto fault;
        ++s->boot_phase;
        return;
    }
    if (s->boot_phase == 2u)
    {
        unsigned index;
        valid[0] = bvpv_cd_record_decode(s->boot_raw[0], &records[0]);
        valid[1] = bvpv_cd_record_decode(s->boot_raw[1], &records[1]);
        if (valid[0] || valid[1])
        {
            index = valid[1] && (!valid[0] || bvpv_cd_sequence_newer(records[1].sequence, records[0].sequence)) ? 1u : 0u;
            s->active = records[index];
            s->record_index = (uint8_t)index;
            s->high_sequence = s->active.sequence;
            if (valid[0] && valid[1] &&
                !bvpv_cd_sequence_newer(records[0].sequence, records[1].sequence) &&
                !bvpv_cd_sequence_newer(records[1].sequence, records[0].sequence) &&
                memcmp(s->boot_raw[0], s->boot_raw[1], BVPV_CD_RECORD_LEN))
            {
                s->last_error = CRC;
                goto done;
            }
            if (s->active.kind == BVPV_CD_RECORD_NO_MEDIA) goto done;
            s->boot_phase = 3u;
            return;
        }
        memset(s->scratch, 0xFF, sizeof(s->boot_raw));
        if (memcmp(s->boot_raw, s->scratch, sizeof(s->boot_raw))) s->last_error = CRC;
        s->boot_phase = 4u;
        return;
    }
    if (s->boot_phase == 3u)
    {
        n = s->active.bytes - s->cursor;
        if (n > sizeof(s->scratch)) n = sizeof(s->scratch);
        addr = BVPV_CD_IMAGE_BASE + s->cursor;
    }
    else
    {
        n = BVPV_CD_RECORD_LEN;
        addr = (uint32_t)(s->boot_phase - 4u) * 4096u;
    }
    result = s->io.read(s->io.ctx, addr, s->scratch, n);
    if (result == BVPV_CD_READ_BUSY) return;
    if (result != BVPV_CD_READ_OK) goto fault;
    if (s->boot_phase == 3u)
    {
        s->crc = bvpv_cd_crc32_update(s->crc, s->scratch, n);
        s->cursor += n;
        if (s->cursor < s->active.bytes) return;
        if (s->crc == s->active.crc32) s->available = true;
        else s->last_error = CRC;
        goto done;
    }
    if (!memcmp(s->scratch, "BVC3", 4) || !memcmp(s->scratch, "BVC2", 4) || !memcmp(s->scratch, "BVCD", 4))
        s->last_error = UNSUPPORTED;
    if (++s->boot_phase < 6u) return;
    goto done;
fault:
    s->fault = true;
    s->last_error = IO_ERROR;
done:
    s->booting = false;
}
bool bvpv_cd_store_ready(const bvpv_cd_store_t *s)
{
    return s->available && !s->fault && !s->session;
}
bvpv_cd_read_result_t bvpv_cd_store_read(bvpv_cd_store_t *s, uint32_t off, uint8_t *buf, uint32_t len)
{
    if (!bvpv_cd_store_ready(s) || !buf || !len || off >= s->active.bytes ||
        len > s->active.bytes - off) return BVPV_CD_READ_ERROR;
    return s->io.read(s->io.ctx, BVPV_CD_IMAGE_BASE + off, buf, len);
}
bvpv_cd_update_state_t bvpv_cd_store_state(const bvpv_cd_store_t *s)
{
    if (s->fault)
        return BVPV_CD_FAULT;
    if (s->cancelled && s->session)
        return BVPV_CD_CLEANUP;
    if (!s->session)
        return BVPV_CD_IDLE;
    if (s->phase >= PH_META_ERASE)
        return BVPV_CD_COMMITTING;
    if (s->phase == PH_VERIFY)
        return BVPV_CD_VERIFYING;
    return s->phase ? BVPV_CD_WRITING : BVPV_CD_RECEIVING;
}
static void reply(bvpv_cd_store_t *s, uint8_t status, uint16_t len)
{
    s->reply[0] = status;
    s->reply_len = len;
    s->reply_ready = true;
    if (status)
        s->last_error = status;
}
static void close_session(bvpv_cd_store_t *s)
{
    s->session = false;
    s->cancelled = false;
    s->phase = PH_NONE;
    s->preparing_begin = false;
    (void)s->io.reserve(s->io.ctx, false);
}
static void fail(bvpv_cd_store_t *s, uint8_t status)
{
    if (status == IO_ERROR)
        s->fault = true;
    close_session(s);
    reply(s, status, 1);
}
void bvpv_cd_store_cancel(bvpv_cd_store_t *s)
{
    if (s->session)
        s->cancelled = true;
}
bool bvpv_cd_store_take_reply(bvpv_cd_store_t *s, uint8_t *out, uint16_t *len)
{
    if (!s->reply_ready)
        return false;
    memcpy(out, s->reply, s->reply_len);
    *len = s->reply_len;
    s->reply_ready = false;
    return true;
}
void bvpv_cd_store_request(bvpv_cd_store_t *s, uint8_t svc, const uint8_t *p, uint16_t n,
                           uint32_t now)
{
    s->reply_ready = false;
    if (svc == 0x0Fu)
    {
        uint8_t *d = s->reply + 1;
        if (n)
        {
            reply(s, LENGTH, 1);
            return;
        }
        d[0] = BVPV_CD_LAYOUT_VERSION;
        d[1] = (uint8_t)bvpv_cd_store_state(s);
        d[2] = s->available ? 1u : 0u;
        d[3] = s->last_error;
        wr32(d + 4, BVPV_CD_SLOT_BYTES);
        wr32(d + 8, s->available ? s->active.bytes : 0);
        wr32(d + 12, s->available ? s->active.crc32 : 0);
        wr32(d + 16, s->high_sequence);
        wr32(d + 20, s->offset);
        reply(s, OK, BVPV_CD_INFO_DATA_LEN + 1u);
        return;
    }
    if (s->phase || s->booting)
    {
        reply(s, BUSY, 1);
        return;
    }
    if (svc == 0x0Du)
    {
        if (n)
        {
            reply(s, LENGTH, 1);
            return;
        }
        if (s->session)
            close_session(s);
        reply(s, OK, 1);
        return;
    }
    if (svc == 0x0Au)
    {
        uint32_t bytes;
        if (!p || n != 8u)
        {
            reply(s, LENGTH, 1);
            return;
        }
        if (s->session)
        {
            reply(s, BUSY, 1);
            return;
        }
        if (s->fault)
        {
            reply(s, STATE, 1);
            return;
        }
        bytes = rd32(p);
        if (!bytes || bytes > BVPV_CD_SLOT_BYTES)
        {
            reply(s, RANGE, 1);
            return;
        }
        if (bytes % BVPV_CD_BLOCK_SIZE)
        {
            reply(s, LENGTH, 1);
            return;
        }
        if (!s->io.reserve(s->io.ctx, true))
        {
            reply(s, BUSY, 1);
            return;
        }
        s->session = true;
        s->cancelled = false;
        s->committed = false;
        s->last_error = 0;
        s->candidate.kind = BVPV_CD_RECORD_IMAGE;
        s->candidate.bytes = bytes;
        s->candidate.crc32 = rd32(p + 4);
        s->candidate.sequence = s->high_sequence + 2u;
        s->candidate.write_count = s->active.write_count + 1u;
        s->target_record = (uint8_t)(1u - s->record_index);
        s->offset = 0;
        s->last_activity = now;
        {
            bvpv_cd_record_t tombstone = {BVPV_CD_RECORD_NO_MEDIA, 0, 0,
                s->high_sequence + 1u, s->active.write_count};
            bvpv_cd_record_encode(s->meta, &tombstone);
        }
        s->preparing_begin = true;
        s->phase = PH_META_ERASE;
        return;
    }
    if (!s->session)
    {
        reply(s, ORDER, 1);
        return;
    }
    if (svc == 0x0Bu)
    {
        if (!p || n != 6u + BVPV_CD_BLOCK_SIZE || rd16(p + 4) != BVPV_CD_BLOCK_SIZE)
        {
            reply(s, LENGTH, 1);
            return;
        }
        if (rd32(p) != s->offset || s->offset >= s->candidate.bytes ||
            BVPV_CD_BLOCK_SIZE > s->candidate.bytes - s->offset)
        {
            reply(s, ORDER, 1);
            return;
        }
        memcpy(s->block, p + 6, BVPV_CD_BLOCK_SIZE);
        s->page_offset = 0;
        s->last_activity = now;
        if (s->offset % 4096u == 0u)
        {
            if (!s->io.erase_start(s->io.ctx, BVPV_CD_IMAGE_BASE + s->offset))
            {
                fail(s, IO_ERROR);
                return;
            }
            s->phase = PH_ERASE_WAIT;
        }
        else
            s->phase = PH_PAGE_START;
        return;
    }
    if (svc == 0x0Cu)
    {
        if (n)
        {
            reply(s, LENGTH, 1);
            return;
        }
        if (s->offset != s->candidate.bytes)
        {
            reply(s, ORDER, 1);
            return;
        }
        s->cursor = 0;
        s->crc = 0;
        s->phase = PH_VERIFY;
        s->last_activity = now;
        return;
    }
    reply(s, UNSUPPORTED, 1);
}
static bool waiting(uint8_t phase)
{
    return phase == PH_ERASE_WAIT || phase == PH_PAGE_WAIT || phase == PH_META_ERASE_WAIT ||
           phase == PH_META_BODY_WAIT || phase == PH_MARK_WAIT;
}
void bvpv_cd_store_task(bvpv_cd_store_t *s, uint32_t now)
{
    uint32_t addr = BVPV_CD_IMAGE_BASE + s->offset;
    uint32_t meta_addr = BVPV_CD_RECORD_BASE(s->target_record);
    bvpv_cd_record_t record;
    if (s->booting)
    {
        boot_task(s);
        return;
    }
    if (!s->session)
        return;
    if (!s->phase && (uint32_t)(now - s->last_activity) >= BVPV_CD_IDLE_TIMEOUT_MS)
        s->cancelled = true;
    if (waiting(s->phase))
    {
        int result = s->io.poll(s->io.ctx);
        if (result > 0)
            return;
        if (result < 0)
        {
            fail(s, IO_ERROR);
            return;
        }
    }
    /* 已发出的提交标志不能撤回；先检查它，再处理断线。 */
    if (s->phase == PH_MARK_WAIT)
    {
        if (s->io.read(s->io.ctx, meta_addr, s->scratch, BVPV_CD_RECORD_LEN) != BVPV_CD_READ_OK ||
            !bvpv_cd_record_decode(s->scratch, &record) ||
            memcmp(s->scratch, s->meta, BVPV_CD_RECORD_LEN))
        {
            fail(s, IO_ERROR);
            return;
        }
        s->active = record;
        s->record_index = s->target_record;
        s->high_sequence = record.sequence;
        if (s->preparing_begin)
        {
            s->available = false;
            s->preparing_begin = false;
            s->target_record = (uint8_t)(1u - s->record_index);
            s->phase = PH_NONE;
            s->last_activity = now;
            if (s->cancelled)
            {
                close_session(s);
                reply(s, STATE, 1);
            }
            else
            {
                wr16(s->reply + 1, BVPV_CD_BLOCK_SIZE);
                reply(s, OK, 3);
            }
            return;
        }
        s->available = true;
        s->committed = true;
        close_session(s);
        s->reply[1] = 1;
        reply(s, OK, 2);
        return;
    }
    if (s->cancelled)
    {
        close_session(s);
        reply(s, STATE, 1);
        return;
    }
    switch (s->phase)
    {
    case PH_NONE:
        break;
    case PH_ERASE_WAIT:
        s->phase = PH_PAGE_START;
        break;
    case PH_PAGE_START:
        if (!s->io.program_start(s->io.ctx, addr + s->page_offset, s->block + s->page_offset, 256))
            fail(s, IO_ERROR);
        else
            s->phase = PH_PAGE_WAIT;
        break;
    case PH_PAGE_WAIT:
        s->page_offset = (uint16_t)(s->page_offset + 256u);
        if (s->page_offset == BVPV_CD_BLOCK_SIZE)
        {
            s->page_offset = 0;
            s->phase = PH_BLOCK_CHECK;
        }
        else
            s->phase = PH_PAGE_START;
        break;
    case PH_BLOCK_CHECK:
        if (s->io.read(s->io.ctx, addr + s->page_offset, s->scratch, 256) != BVPV_CD_READ_OK)
        {
            fail(s, IO_ERROR);
            break;
        }
        if (memcmp(s->scratch, s->block + s->page_offset, 256))
        {
            fail(s, CRC);
            break;
        }
        s->page_offset = (uint16_t)(s->page_offset + 256u);
        if (s->page_offset == BVPV_CD_BLOCK_SIZE)
        {
            s->offset += BVPV_CD_BLOCK_SIZE;
            s->phase = PH_NONE;
            s->last_activity = now;
            wr32(s->reply + 1, s->offset);
            reply(s, OK, 5);
        }
        break;
    case PH_VERIFY:
    {
        uint32_t n = s->candidate.bytes - s->cursor;
        if (n > sizeof(s->scratch))
            n = sizeof(s->scratch);
        if (s->io.read(s->io.ctx, BVPV_CD_IMAGE_BASE + s->cursor, s->scratch, n) != BVPV_CD_READ_OK)
        {
            fail(s, IO_ERROR);
            break;
        }
        s->crc = bvpv_cd_crc32_update(s->crc, s->scratch, n);
        s->cursor += n;
        if (s->cursor == s->candidate.bytes)
        {
            if (s->crc != s->candidate.crc32)
                fail(s, CRC);
            else
            {
                bvpv_cd_record_encode(s->meta, &s->candidate);
                s->phase = PH_META_ERASE;
            }
        }
        break;
    }
    case PH_META_ERASE:
        if (!s->io.erase_start(s->io.ctx, meta_addr))
            fail(s, IO_ERROR);
        else
            s->phase = PH_META_ERASE_WAIT;
        break;
    case PH_META_ERASE_WAIT:
        s->phase = PH_META_BODY;
        break;
    case PH_META_BODY:
        if (!s->io.program_start(s->io.ctx, meta_addr, s->meta, BVPV_CD_COMMIT_OFFSET))
            fail(s, IO_ERROR);
        else
            s->phase = PH_META_BODY_WAIT;
        break;
    case PH_META_BODY_WAIT:
        s->phase = PH_META_CHECK;
        break;
    case PH_META_CHECK:
        if (s->io.read(s->io.ctx, meta_addr, s->scratch, BVPV_CD_RECORD_LEN) != BVPV_CD_READ_OK)
        {
            fail(s, IO_ERROR);
            break;
        }
        if (memcmp(s->scratch, s->meta, BVPV_CD_RECORD_LEN))
        {
            fail(s, CRC);
            break;
        }
        s->meta[BVPV_CD_COMMIT_OFFSET] = BVPV_CD_META_VALID;
        if (!s->io.program_start(s->io.ctx, meta_addr + BVPV_CD_COMMIT_OFFSET, s->meta + BVPV_CD_COMMIT_OFFSET, 1))
            fail(s, IO_ERROR);
        else
            s->phase = PH_MARK_WAIT;
        break;
    default:
        fail(s, IO_ERROR);
        break;
    }
}
