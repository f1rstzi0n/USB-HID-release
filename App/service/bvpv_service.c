#include "bvpv_service.h"

#include "bvpv_errcode.h"
#include "bvpv_svc_internal.h"

/* 内部静态上下文（单设备，嵌入式惯例）。 */
static bvpv_service_ctx_t *s_ctx;
static const bvpv_msc_backend_t *s_msc;
static const bvpv_fw_backend_t *s_fw;
static const bvpv_catalog_backend_t *s_catalog;
static bool s_pending_catalog;
void bvpv_service_set_catalog_backend(const bvpv_catalog_backend_t *backend) { s_catalog = backend; }
bool bvpv_service_catalog_supported(void) { return s_catalog && s_catalog->request; }
bool bvpv_service_catalog_committed(void) {
    return s_catalog && s_catalog->committed && s_catalog->committed();
}
void bvpv_service_task(void) {
    if (s_catalog && s_catalog->task) s_catalog->task();
    if (s_msc && s_msc->task) s_msc->task();
}
void bvpv_service_set_fw_backend(const bvpv_fw_backend_t *fw) { s_fw = fw; }
bool bvpv_service_fw_supported(void) { return s_fw && s_fw->request; }
void bvpv_service_reply_complete(uint8_t svc, bool delivered) {
    if (s_catalog && s_catalog->reply_complete) s_catalog->reply_complete(svc, delivered);
    if (s_fw && s_fw->reply_complete) s_fw->reply_complete(svc, delivered);
}
static bool s_pending;
static uint8_t s_pending_svc, s_pending_flags;
static uint16_t s_pending_tid;
void bvpv_service_set_msc_backend(const bvpv_msc_backend_t *backend) { s_msc=backend; }
bool bvpv_service_pending(void) { return s_pending; }
void bvpv_service_cancel(void) {
    if (s_catalog && s_catalog->cancel) s_catalog->cancel();
    if (s_msc && s_msc->cancel) s_msc->cancel();
    if (s_fw && s_fw->cancel) s_fw->cancel();
    s_pending=false;
}
size_t bvpv_service_poll(uint8_t *out, uint16_t cap)
{
    uint8_t payload[27];
    uint16_t len=0;
    if (s_pending && s_pending_catalog) {
        uint8_t *data;
        uint16_t room;
        if (!s_catalog || !s_catalog->poll || cap <= BVPV_FRAME_OVERHEAD) return 0;
        data = out + BVPV_FRAME_HDR_LEN;
        room = (uint16_t)(cap - BVPV_FRAME_OVERHEAD);
        if (!s_catalog->poll(data, &len, room)) return 0;
        s_pending = false;
        if (!len || len > room) return 0;
        return bvpv_frame_encode(out, s_pending_svc, s_pending_flags, s_pending_tid, data, len);
    }
    if(!s_pending || !s_msc || cap<BVPV_FRAME_OVERHEAD+sizeof(payload)) return 0;
    if(!s_msc->poll(payload,&len)) return 0;
    s_pending=false;
    if(!len || len>sizeof(payload)) return 0;
    return bvpv_frame_encode(out,s_pending_svc,s_pending_flags,s_pending_tid,payload,len);
}

/* 注册服务上下文（目录 + 设备身份）为内部静态指针，供 dispatch 与各处理器使用。
 * 注意：ctx 必须是静态/长期存储（覆盖整个运行期），不可为栈上临时变量。 */
void bvpv_service_init(bvpv_service_ctx_t *ctx)
{
    s_ctx = ctx;
    s_msc = 0;
    s_fw = 0;
    s_catalog = 0;
    s_pending_catalog = false;
    s_pending = false;
}

/* 服务分发：把已解码的请求帧按 SVC 路由到对应处理器，集中构造并返回完整应答帧。
 * 集中处理：SVC 回显、FLAGS 置方向位、TID 回显、载荷首字节=状态码、帧编码（含 CRC16）。
 * 处理器只填"状态码之后"的数据并返回状态码。未定义/未实现 SVC → 0x09。
 * 返回应答帧长；参数非法/容量不足返回 0（调用方不回包）。out 至少需 BVPV_MAX_FRAME。 */
size_t bvpv_service_dispatch(const bvpv_frame_t *req, uint8_t *out, uint16_t out_cap)
{
    uint8_t *pload;
    uint16_t pcap;
    uint16_t dlen = 0u;
    uint8_t  status;
    uint8_t  flags;

    if ((s_ctx == 0) || (req == 0) || (out == 0)) {
        return 0u;
    }
    if (out_cap < (uint16_t)(BVPV_FRAME_OVERHEAD + 1u)) {
        return 0u;
    }

    /* 旧版或保留标志不进入任何业务服务；应答声明本机 v4，不降级执行。 */
    if (req->flags != BVPV_PROTO_VERSION) {
        const uint8_t mismatch = BVPV_ERR_STATE;
        return bvpv_frame_encode(out, req->svc,
            BVPV_FLAGS_DIR_RESPONSE | BVPV_PROTO_VERSION, req->tid, &mismatch, 1u);
    }
    /* 载荷区在帧头之后；首字节为状态码，数据区在其后 */
    pload = out + BVPV_FRAME_HDR_LEN;
    pcap  = (uint16_t)(out_cap - BVPV_FRAME_OVERHEAD);   /* 含状态字节的载荷容量 */

    if(s_pending) return 0;
    if (s_catalog) {
        bool maintenance = s_catalog->maintenance && s_catalog->maintenance();
        bool new_catalog_service = req->svc >= BVPV_SVC_CATALOG_INFO &&
                                   req->svc <= BVPV_SVC_CATALOG_UPDATE_ABORT;
        bool ready = s_catalog->ready && s_catalog->ready();
        bool description = req->svc == BVPV_SVC_TREE_PULL ||
                           req->svc == BVPV_SVC_ATTR_EXT_READ ||
                           req->svc == BVPV_SVC_ATTR_SNAPSHOT_PULL ||
                           (req->svc == BVPV_SVC_ERRTEXT_PULL && ready);
        bool ordinary = (req->svc >= BVPV_SVC_TREE_PULL && req->svc <= BVPV_SVC_STATE_READ) ||
                        req->svc == BVPV_SVC_ATTR_CORE_READ ||
                        req->svc == BVPV_SVC_CHANGE_QUERY ||
                        req->svc == BVPV_SVC_RUNTIME_SNAPSHOT_PULL;
        if ((ordinary && (maintenance || !ready)) ||
            (maintenance && (req->svc == BVPV_SVC_ENTER_BOOTLOADER ||
                             req->svc == BVPV_SVC_MSC_UPDATE_BEGIN))) {
            const uint8_t error = maintenance ? BVPV_ERR_BUSY : BVPV_ERR_STATE;
            return bvpv_frame_encode(out, req->svc,
                (uint8_t)(req->flags | BVPV_FLAGS_DIR_RESPONSE), req->tid, &error, 1u);
        }
        if ((new_catalog_service && req->svc != BVPV_SVC_RUNTIME_SNAPSHOT_PULL) || description) {
            uint16_t length = 0;
            if (!s_catalog->request(req->svc, req->payload, req->plen, pload, &length, pcap)) {
                s_pending = true; s_pending_catalog = true; s_pending_svc = req->svc;
                s_pending_flags = (uint8_t)(req->flags | BVPV_FLAGS_DIR_RESPONSE);
                s_pending_tid = req->tid;
                return 0;
            }
            if (!length || length > pcap) return 0;
            return bvpv_frame_encode(out, req->svc,
                (uint8_t)(req->flags | BVPV_FLAGS_DIR_RESPONSE), req->tid, pload, length);
        }
    }
    if((req->svc>=BVPV_SVC_MSC_UPDATE_BEGIN && req->svc<=BVPV_SVC_MSC_UPDATE_ABORT) ||
       req->svc==BVPV_SVC_MSC_INFO) {
        uint8_t payload[27];
        uint16_t len=0;
        if(s_msc && pcap>=sizeof(payload)) {
            if(!s_msc->request(req->svc,req->payload,req->plen,payload,&len)) {
                s_pending=true; s_pending_catalog=false; s_pending_svc=req->svc;
                s_pending_flags=(uint8_t)(req->flags|BVPV_FLAGS_DIR_RESPONSE);
                s_pending_tid=req->tid;
                return 0;
            }
            if(!len || len>sizeof(payload)) return 0;
            return bvpv_frame_encode(out,req->svc,(uint8_t)(req->flags|BVPV_FLAGS_DIR_RESPONSE),req->tid,payload,len);
        }
    }
    if (req->svc == BVPV_SVC_ENTER_BOOTLOADER || req->svc == BVPV_SVC_FW_INFO) {
        uint16_t len = 0;
        if (s_fw && s_fw->request && pcap >= 149u) {
            s_fw->request(req->svc, req->payload, req->plen, pload, &len);
            if (len < 1u || len > 149u) return 0;
            return bvpv_frame_encode(out, req->svc,
                (uint8_t)(req->flags | BVPV_FLAGS_DIR_RESPONSE), req->tid, pload, len);
        }
    }
    switch (req->svc) {
    case BVPV_SVC_HELLO:
        status = svc_hello(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_TREE_PULL:
        status = svc_tree_pull(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_ATTR_EXT_READ:
        status = svc_attr_ext_read(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_ERRTEXT_PULL:
        status = svc_errtext_pull(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_VALUE_READ:
        status = svc_value_read(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_VALUE_WRITE:
        status = svc_value_write(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_VALUE_READ_BATCH:
        status = svc_value_read_batch(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_STATE_READ:
        status = svc_state_read(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_ATTR_CORE_READ:
        status = svc_attr_core_read(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_ATTR_SNAPSHOT_PULL:
        status = svc_attr_snapshot_pull(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_CHANGE_QUERY:
        status = svc_change_query(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_RUNTIME_SNAPSHOT_PULL:
        status = svc_runtime_snapshot_pull(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    case BVPV_SVC_ECHO:
        status = svc_echo(s_ctx, req, pload + 1, &dlen, (uint16_t)(pcap - 1u));
        break;
    default:
        /* 未定义服务或未绑定对应 backend。 */
        status = BVPV_ERR_NOT_SUPPORTED;
        dlen   = 0u;
        break;
    }

    pload[0] = status;
    flags = (uint8_t)(req->flags | BVPV_FLAGS_DIR_RESPONSE);
    return bvpv_frame_encode(out, req->svc, flags, req->tid, pload, (uint16_t)(1u + dlen));
}
