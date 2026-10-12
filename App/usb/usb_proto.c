#include "usb_proto.h"
#include "usb_io.h"
#include "usb_port.h"
#include "bvpv_usb_frag.h"
#include <string.h>
#define TX_QUEUE_LEN 70u
#define TX_WATCHDOG_MS 500u
static const usb_proto_backend_t *s_backend;
static bvpv_frag_rx_t s_rx;
static bvpv_frag_tx_t s_tx;
static uint8_t s_frame[BVPV_MAX_FRAME], s_queue[TX_QUEUE_LEN][BVPV_REPORT_LEN];
static uint8_t s_head, s_tail, s_count, s_svc, s_policy, s_reenum;
static uint16_t s_request_len;
static uint32_t s_epoch, s_ticket, s_token, s_tx_start, s_reenum_start, s_connect_serial;
static bool s_request, s_busy, s_in_flight, s_hold_rx;
static void clear_response(void)
{
    usb_io_tx_cancel(s_token);
    s_head = s_tail = s_count = 0;
    s_busy = s_in_flight = false;
}
void usb_proto_disconnected(void)
{
    if (s_busy && s_backend && s_backend->reply_complete) s_backend->reply_complete(s_svc, false);
    if (s_backend && s_backend->cancel) s_backend->cancel();
    clear_response();
    bvpv_frag_rx_init(&s_rx); s_request = false; s_policy = 0; s_ticket = 0; s_hold_rx = false;
}
void usb_proto_init_backend(const usb_proto_backend_t *backend)
{
    s_backend = backend; s_epoch = usb_io_epoch(); s_ticket = 0; s_token = 0;
    s_reenum = 0; s_policy = 0; s_hold_rx = false; s_request = s_busy = s_in_flight = false;
    s_head = s_tail = s_count = 0;
    bvpv_frag_rx_init(&s_rx); bvpv_frag_tx_init(&s_tx);
}
static void emit(const uint8_t *report, void *ctx)
{
    (void)ctx;
    if (s_count < TX_QUEUE_LEN) {
        memcpy(s_queue[s_head], report, BVPV_REPORT_LEN);
        s_head = (uint8_t)((s_head + 1u) % TX_QUEUE_LEN); ++s_count;
    }
}
static void queue_response(size_t len)
{
    if (!len || len > sizeof(s_frame)) return;
    s_head = s_tail = s_count = 0;
    if (bvpv_frag_tx_send(&s_tx, s_frame, (uint16_t)len, emit, 0)) {
        s_svc = s_frame[0]; s_busy = true; s_tx_start = usb_port_millis();
        if (++s_token == 0) ++s_token; /* 0 保留为已取消令牌。 */
        s_policy = s_backend->reply_policy ? s_backend->reply_policy(s_frame, len) : 0;
    }
}
static void response_finished(bool delivered)
{
    uint8_t policy = s_policy;
    clear_response(); s_policy = 0;
    s_hold_rx = delivered && (policy & USB_REPLY_KEEP_RX_CLOSED);
    if (s_backend && s_backend->reply_complete) s_backend->reply_complete(s_svc, delivered);
    if (delivered && (policy & USB_REPLY_REENUMERATE)) {
        s_reenum = 1; s_reenum_start = usb_port_millis();
    }
}
static void take_completion(void)
{
    usb_io_done_t done;
    if (usb_io_done_take(&done) && done.epoch == s_epoch && done.epoch == usb_io_epoch() && done.token == s_token && s_in_flight) {
        s_in_flight = false;
        if (!done.delivered || !s_count) {
            s_ticket = done.next_ticket;
            response_finished(done.delivered);
        }
    }
}
void usb_proto_task(void)
{
    uint32_t now, epoch, dispatched_epoch, disconnected_ms;
    uint16_t length;
    unsigned budget = USB_IO_RX_SLOTS;
    size_t response;
    usb_io_rx_t report;
    bvpv_frame_t request;
    if (!s_backend) return;
    epoch = usb_io_epoch();
    if (epoch != s_epoch) { usb_proto_disconnected(); s_epoch = epoch; }
    take_completion();
    if (usb_io_rx_overflowed()) { bvpv_frag_rx_init(&s_rx); s_ticket = 0; }
    now = usb_port_millis();
    if (s_reenum == 1 && (uint32_t)(now - s_reenum_start) >= 100u) {
        s_connect_serial = usb_io_connect_request(false); s_reenum = 2;
    }
    if (s_reenum == 2 && usb_io_connect_applied(s_connect_serial, &disconnected_ms) &&
        (uint32_t)(now - disconnected_ms) >= 250u) {
        (void)usb_io_connect_request(true); s_reenum = 0;
    }
    if (s_reenum || !usb_io_mounted()) {
        if (s_backend->task) s_backend->task();
        return;
    }
    while (budget-- && !s_request && usb_io_rx_take(&report)) {
        /* 完成发布与 RX 扫描之间可发生抢占；先消费新窗口的完成证据。 */
        take_completion();
        if (report.epoch != s_epoch || report.epoch != usb_io_epoch() || s_busy ||
            (s_backend->pending && s_backend->pending())) continue;
        if (report.ticket != s_ticket) { bvpv_frag_rx_init(&s_rx); s_ticket = report.ticket; }
        now = usb_port_millis();
        if ((uint32_t)(now - report.received_ms) >= 200u) { bvpv_frag_rx_init(&s_rx); continue; }
        length = 0;
        if (bvpv_frag_rx_feed(&s_rx, report.report, report.received_ms, &length)) {
            if (usb_io_rx_close(report.epoch, report.ticket)) {
                s_request_len = length; s_request = true;
            } else bvpv_frag_rx_init(&s_rx);
        }
    }
    if (!s_request && s_backend->task) s_backend->task();
    bvpv_frag_rx_check_timeout(&s_rx, now);
    if (!s_busy) {
        if (s_backend->pending && s_backend->pending()) {
            if (s_backend->poll) queue_response(s_backend->poll(s_frame, sizeof(s_frame)));
        } else if (s_request) {
            dispatched_epoch = s_epoch;
            if (dispatched_epoch == usb_io_epoch() && s_backend->dispatch && bvpv_frame_decode(s_rx.buf, s_request_len, &request)) {
                response = s_backend->dispatch(&request, s_frame, sizeof(s_frame));
                if (dispatched_epoch == usb_io_epoch()) queue_response(response);
            }
            s_request = false;
        }
    }
    if (s_count && !s_in_flight && usb_io_tx_submit(s_queue[s_tail], s_epoch, s_token,
            s_tx_start + TX_WATCHDOG_MS, s_count == 1,
            !(s_policy & (USB_REPLY_REENUMERATE | USB_REPLY_KEEP_RX_CLOSED)))) {
        s_tail = (uint8_t)((s_tail + 1u) % TX_QUEUE_LEN); --s_count; s_in_flight = true;
    }
    now = usb_port_millis();
    if (s_busy && (uint32_t)(now - s_tx_start) >= TX_WATCHDOG_MS) response_finished(false);
    if (!s_hold_rx && !s_busy && !s_request && !s_reenum && !(s_backend->pending && s_backend->pending()))
        s_ticket = usb_io_rx_open();
}
