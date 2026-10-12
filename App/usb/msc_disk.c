/* MSC 只读光驱：ISO9660 镜像存于外置 W25Q64，主机侧呈现为一张 CD-ROM。
 *
 * 设备类型是 SCSI PDT 0x05（CD/DVD），逻辑块 2048B（ISO9660 扇区），只读。
 * 内容读取经 src/content/content_store.c 走 SPI2 → W25Q64；没有任何内容驻留 RAM。
 *
 * 为什么除 TinyUSB 内建命令外还要自己实现四条 SCSI 命令：
 * TinyUSB 的 msc_device.c 只内建 TEST_UNIT_READY / INQUIRY / MODE_SENSE_6 /
 * REQUEST_SENSE / READ_CAPACITY_10 / READ_FORMAT_CAPACITY / START_STOP_UNIT /
 * PREVENT_ALLOW_MEDIUM_REMOVAL / READ_10 / WRITE_10。这些是**磁盘**命令集；
 * 光驱还要求 MMC 专属的几条，Windows 的 cdrom.sys 会发，落到 tud_msc_scsi_cb：
 *   - READ_TOC(0x43)                        盘目：几条轨道、lead-out 在哪
 *   - GET_CONFIGURATION(0x46)               当前 profile（CD-ROM）
 *   - GET_EVENT_STATUS_NOTIFICATION(0x4A)   介质变化通知（本盘恒定在位，答"无事件"）
 *   - MODE_SENSE_10(0x5A)                   部分主机会用它取写保护状态
 *
 * 并发：MSC 回调跑在 tud_task() 的主循环上下文（TinyUSB 的 MSC 驱动 xfer_isr 为
 * NULL，XFER_COMPLETE 走 queue_event），所以同步轮询 SPI 安全；但 SPI2 与电机接口
 * 共享，拿不到总线时返回 TUD_MSC_RET_BUSY，TinyUSB 会用同样参数重试。 */
#include "tusb.h"

#if CFG_TUD_MSC

#include <string.h>

#include "bvpv_cd_meta.h"
#include "usb_media.h"

/* 光驱专属 SCSI 命令码（MMC-3）。TinyUSB 的 msc.h 未定义这几条。 */
#define SCSI_CMD_READ_TOC                      0x43u
#define SCSI_CMD_GET_CONFIGURATION             0x46u
#define SCSI_CMD_GET_EVENT_STATUS_NOTIFICATION 0x4Au
#define SCSI_CMD_MODE_SENSE_10                 0x5Au

/* MMC-3 当前 profile：0x0008 = CD-ROM。 */
#define MMC_PROFILE_CD_ROM                     0x0008u

/* ---- INQUIRY（v2 回调） ---- */
/* 必须用 v2：内建处理只 memset 结构体再填 is_removable/version/response_data_format，
 * **不填 peripheral_device_type**（留 0 = 磁盘）。设备类型字节是光驱与 U 盘的分水岭。
 * 实现了 v2 就不需要 v1——两处字符串真源只会分叉。 */
uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *inquiry_resp, uint32_t bufsize)
{
    (void)lun;
    (void)bufsize;

    inquiry_resp->peripheral_device_type = SCSI_PDT_CD_DVD;   /* 0x05 */
    inquiry_resp->is_removable           = 1;
    memcpy(inquiry_resp->vendor_id,  "AURASKY ", 8u);
    memcpy(inquiry_resp->product_id, "BV500-USB Disc  ", 16u);
    memcpy(inquiry_resp->product_rev, "1.0 ", 4u);
    return sizeof(scsi_inquiry_resp_t);
}

/* ---- 介质在位 ---- */
/* 返回 false 时 TinyUSB 自动置 sense = MEDIUM NOT PRESENT，主机侧表现为"空光驱"
 * （驱动器图标在、盘不在）。 */
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return usb_media_ready();
}

/* ---- 容量 ---- */
/* **必须始终返回非零**：TinyUSB 见到 block_count 或 block_size 为 0 就直接让
 * READ_CAPACITY / READ_FORMAT_CAPACITY 失败（设备变成坏盘，而不是空盘）。
 * 有介质时报实际块数；空光驱用镜像区上限兜底，由 test_unit_ready 表达无介质。 */
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    uint32_t blocks = usb_media_block_count();
    (void)lun;
    *block_count = blocks ? blocks : BVPV_CD_SLOT_BLOCKS;
    *block_size  = BVPV_CD_BLOCK_SIZE;      /* 2048 */
}

/* ---- 只读 ---- */
bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return false;   /* 只读光盘：MODE_SENSE_6 的 WP 位据此上报 */
}

/* ---- READ_10：唯一的读取数据路径 ---- */
/* TinyUSB 把一次 READ_10 拆成 ≤64B 的分片依次调用本回调（lba/offset 已按已传字节
 * 换算好），所以这里每次只搬一小段。offset 是**块内**偏移，不是镜像内偏移。 */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize)
{
    uint32_t blocks = usb_media_block_count();
    uint32_t image_bytes = blocks * BVPV_CD_BLOCK_SIZE;
    uint32_t iso_offset;
    bvpv_cd_read_result_t st;

    (void)lun;

    if (!blocks || lba >= blocks) return TUD_MSC_RET_ERROR;
    iso_offset = bvpv_cd_lba_to_image_offset(lba, offset);
    if ((iso_offset == BVPV_CD_OFFSET_INVALID) || (iso_offset >= image_bytes) ||
        (bufsize > (image_bytes - iso_offset))) {
        return TUD_MSC_RET_ERROR;   /* 越界：超出镜像实际长度 */
    }

    st = usb_media_read(iso_offset, (uint8_t *)buffer, bufsize);
    if (st == BVPV_CD_READ_BUSY) {
        return TUD_MSC_RET_BUSY;    /* SPI2 被电机侧占用：TinyUSB 稍后原样重试 */
    }
    if (st != BVPV_CD_READ_OK) {
        return TUD_MSC_RET_ERROR;
    }
    return (int32_t)bufsize;
}

/* WRITE_10：只读盘。实际上不会走到——TinyUSB 在 is_writable_cb 为 false 时已用
 * sense = DATA PROTECT 挡掉。留着是为了语义完整（谁改坏了不该有路径可写）。 */
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize)
{
    (void)lun; (void)lba; (void)offset; (void)buffer; (void)bufsize;
    return TUD_MSC_RET_ERROR;
}

/* 允许装载/弹出（无实际动作）：盘是焊死在 Flash 里的，弹出只是主机侧的 UI 语义。 */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

/* ---- 光驱专属命令 ---- */

/* 写一条 TOC 轨道描述符（8 字节），返回写入长度。 */
static uint32_t toc_track(uint8_t *p, uint8_t track_no, uint32_t lba)
{
    memset(p, 0, 8u);
    p[1] = 0x14u;                       /* ADR=1（带 Q 子通道）+ Control=4（数据轨） */
    p[2] = track_no;                    /* 曲目号；lead-out 用 0xAA */
    p[4] = (uint8_t)((lba >> 24) & 0xFFu);   /* 起始地址，大端 LBA */
    p[5] = (uint8_t)((lba >> 16) & 0xFFu);
    p[6] = (uint8_t)((lba >> 8) & 0xFFu);
    p[7] = (uint8_t)(lba & 0xFFu);
    return 8u;
}

/* READ_TOC（0x43）格式 0：单轨数据盘。响应 = 4 字节头 + 轨道描述符 + lead-out 描述符。
 * lead-out 起始地址即全盘扇区数（主机据此算容量）。 */
static int32_t scsi_read_toc(const uint8_t *cdb, uint8_t *buf, uint32_t bufsize)
{
    uint8_t  toc[20];
    uint32_t n;
    uint32_t alloc;
    uint8_t  start_track = cdb[6];
    uint32_t total_blocks;

    if ((cdb[2] & 0x0Fu) != 0u) {
        return TUD_MSC_RET_ERROR;       /* 只支持格式 0（TOC）；多会话格式 1 用不上 */
    }
    total_blocks = usb_media_block_count();
    if (!total_blocks) return TUD_MSC_RET_ERROR;

    memset(toc, 0, sizeof(toc));
    if (start_track == 0xAAu) {
        /* 只问 lead-out */
        toc[2] = 0u;  toc[3] = 0u;
        n = 4u + toc_track(toc + 4u, 0xAAu, total_blocks);
    } else {
        toc[2] = 1u;  toc[3] = 1u;      /* 首轨 = 末轨 = 1 */
        n = 4u + toc_track(toc + 4u, 1u, 0u);          /* 轨道 1 从 LBA 0 起 */
        n += toc_track(toc + n, 0xAAu, total_blocks);  /* lead-out */
    }
    toc[0] = (uint8_t)(((n - 2u) >> 8) & 0xFFu);   /* TOC 数据长度 = 其后字节数 */
    toc[1] = (uint8_t)((n - 2u) & 0xFFu);

    /* 主机给的分配长度只应截短，不应越界写端点缓冲 */
    alloc = ((uint32_t)cdb[7] << 8) | (uint32_t)cdb[8];
    if (alloc < n) {
        n = alloc;
    }
    if (n > bufsize) {
        n = bufsize;
    }
    if (n == 0u) {
        return TUD_MSC_RET_ERROR;
    }
    memcpy(buf, toc, n);
    return (int32_t)n;
}

/* GET_CONFIGURATION（0x46）：回报"无特性列表 + 当前 profile = CD-ROM"。
 * 8 字节固定头：[0..3] 特性数据长度=0，[4..5] 保留，[6..7] 当前 profile（大端）。 */
static int32_t scsi_get_configuration(const uint8_t *cdb, uint8_t *buf, uint32_t bufsize)
{
    uint8_t  resp[8];
    uint32_t n;
    uint32_t alloc;

    memset(resp, 0, sizeof(resp));
    resp[6] = (uint8_t)((MMC_PROFILE_CD_ROM >> 8) & 0xFFu);
    resp[7] = (uint8_t)(MMC_PROFILE_CD_ROM & 0xFFu);

    n = sizeof(resp);
    alloc = ((uint32_t)cdb[7] << 8) | (uint32_t)cdb[8];
    if (alloc < n) {
        n = alloc;
    }
    if (n > bufsize) {
        n = bufsize;
    }
    if (n == 0u) {
        return TUD_MSC_RET_ERROR;
    }
    memcpy(buf, resp, n);
    return (int32_t)n;
}

/* GET_EVENT_STATUS_NOTIFICATION：消费更新期间的介质移除/恢复事件。
 * 主机请求媒体事件类 bit4 时返回 class=4；无新事件则设置 NEA。 */
static int32_t scsi_get_event_status(const uint8_t *cdb, uint8_t *buf, uint32_t bufsize)
{
    uint8_t  resp[8];
    uint32_t n;
    uint32_t alloc;
    bool media_requested = (cdb[4] & 0x10u) != 0;

    memset(resp, 0, sizeof(resp));
    resp[1] = 0x06u;                            /* 事件数据长度 = 其后 6 字节 */
    resp[2] = 0x80u;
    resp[3] = 0x10u;
    if(media_requested) {
        bool ready;
        uint8_t event=usb_media_event(&ready);
        resp[2]=(uint8_t)(0x04u | (event?0u:0x80u));
        resp[4]=event;
        resp[5]=ready?0x02u:0u;
    }

    n = sizeof(resp);
    alloc = ((uint32_t)cdb[7] << 8) | (uint32_t)cdb[8];
    if (alloc < n) {
        n = alloc;
    }
    if (n > bufsize) {
        n = bufsize;
    }
    if (n == 0u) {
        return TUD_MSC_RET_ERROR;
    }
    memcpy(buf, resp, n);
    return (int32_t)n;
}

/* MODE_SENSE_10（0x5A）：与内建的 MODE_SENSE_6 同义，只是 10 字节 CDB 版。
 * 只回 8 字节头、不含块描述符与任何页；[3] bit7 = WP 表达写保护。 */
static int32_t scsi_mode_sense10(uint8_t *buf, uint32_t bufsize)
{
    uint8_t  resp[8];
    uint32_t n;

    memset(resp, 0, sizeof(resp));
    resp[1] = 0x06u;        /* 模式数据长度 = 其后字节数 */
    resp[3] = 0x80u;        /* bit7 = WP：只读介质 */

    n = sizeof(resp);
    if (n > bufsize) {
        n = bufsize;
    }
    if (n == 0u) {
        return TUD_MSC_RET_ERROR;
    }
    memcpy(buf, resp, n);
    return (int32_t)n;
}

/* 其余 SCSI 命令（内建未覆盖的）：不支持即失败。
 * 注意 bufsize 是本回调可写的字节数上限；返回长度必须 ≤ 它，否则越界写端点缓冲。 */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)lun;

    switch (scsi_cmd[0]) {
    case SCSI_CMD_READ_TOC:
        return scsi_read_toc(scsi_cmd, (uint8_t *)buffer, bufsize);
    case SCSI_CMD_GET_CONFIGURATION:
        return scsi_get_configuration(scsi_cmd, (uint8_t *)buffer, bufsize);
    case SCSI_CMD_GET_EVENT_STATUS_NOTIFICATION:
        return scsi_get_event_status(scsi_cmd, (uint8_t *)buffer, bufsize);
    case SCSI_CMD_MODE_SENSE_10:
        return scsi_mode_sense10((uint8_t *)buffer, bufsize);
    default:
        return TUD_MSC_RET_ERROR;
    }
}

#endif /* CFG_TUD_MSC */
