#include "usb_io.h"
#include "usb_port.h"
#include "tusb.h"
#include <string.h>
typedef struct {
    uint8_t report[64];
    uint32_t epoch, token, deadline;
    bool last, reopen;
} tx_report_t;
static usb_io_rx_t s_rx[USB_IO_RX_SLOTS];
static uint8_t s_head, s_tail, s_count;
static uint32_t s_epoch, s_ticket, s_token;
static bool s_mounted, s_gate, s_overflow;
static tx_report_t s_tx, s_active;
static bool s_tx_pending, s_in_flight, s_done_pending;
static usb_io_done_t s_done;
static uint32_t s_connect_serial, s_connect_epoch, s_applied_serial, s_applied_ms;
static bool s_connect_pending, s_connect_on;
volatile uint32_t g_usb_rx_overflows, g_usb_rx_high_water;
void usb_io_init(void)
{
    uint32_t saved = usb_io_enter_critical();
    s_epoch = 1; s_ticket = 0; s_token = 0;
    s_head = s_tail = s_count = 0;
    s_gate = s_mounted = s_overflow = false;
    s_tx_pending = s_in_flight = s_done_pending = s_connect_pending = false;
    s_connect_serial = s_applied_serial = 0;
    g_usb_rx_overflows = g_usb_rx_high_water = 0;
    usb_io_leave_critical(saved);
}
uint32_t usb_io_epoch(void)
{
    uint32_t saved = usb_io_enter_critical();
    uint32_t value = s_epoch;
    usb_io_leave_critical(saved);
    return value;
}
bool usb_io_mounted(void)
{
    uint32_t saved = usb_io_enter_critical();
    bool value = s_mounted;
    usb_io_leave_critical(saved);
    return value;
}
void usb_io_link_reset(void)
{
    uint32_t saved = usb_io_enter_critical();
    ++s_epoch; s_gate = s_mounted = false;
    s_head = s_tail = s_count = 0;
    s_tx_pending = s_done_pending = false;
    s_token = 0;
    usb_io_leave_critical(saved);
}
void usb_io_link_changed(bool mounted)
{
    uint32_t saved = usb_io_enter_critical();
    ++s_epoch; s_gate = false; s_mounted = mounted;
    s_head = s_tail = s_count = 0;
    s_tx_pending = s_done_pending = s_in_flight = false;
    s_token = 0;
    usb_io_leave_critical(saved);
}
uint32_t usb_io_rx_open(void)
{
    uint32_t saved = usb_io_enter_critical();
    uint32_t ticket = 0;
    if (s_mounted) {
        if (!s_gate) { ++s_ticket; s_gate = true; }
        ticket = s_ticket;
    }
    usb_io_leave_critical(saved);
    return ticket;
}
bool usb_io_rx_close(uint32_t epoch, uint32_t ticket)
{
    uint32_t saved = usb_io_enter_critical();
    bool claimed = epoch == s_epoch && ticket == s_ticket && s_gate && s_mounted;
    if (claimed) {
        s_gate = false; s_head = s_tail = s_count = 0;
    }
    usb_io_leave_critical(saved);
    return claimed;
}
void usb_io_report_received(const uint8_t *report, uint16_t len)
{
    uint32_t saved, now;
    usb_io_rx_t *slot;
    if (!report || len != USB_IO_REPORT_SIZE) return;
    now = usb_port_millis();
    saved = usb_io_enter_critical();
    if (s_gate && s_mounted) {
        if (s_count == USB_IO_RX_SLOTS) {
            s_gate = false; s_overflow = true;
            s_head = s_tail = s_count = 0;
            ++g_usb_rx_overflows;
        } else {
            slot = &s_rx[s_head];
            slot->epoch = s_epoch; slot->ticket = s_ticket; slot->received_ms = now;
            memcpy(slot->report, report, USB_IO_REPORT_SIZE);
            s_head = (uint8_t)((s_head + 1u) % USB_IO_RX_SLOTS);
            ++s_count;
            if (s_count > g_usb_rx_high_water) g_usb_rx_high_water = s_count;
        }
    }
    usb_io_leave_critical(saved);
}
bool usb_io_rx_take(usb_io_rx_t *out)
{
    uint32_t saved = usb_io_enter_critical();
    bool have = s_count != 0;
    if (have) {
        *out = s_rx[s_tail];
        s_tail = (uint8_t)((s_tail + 1u) % USB_IO_RX_SLOTS);
        --s_count;
    }
    usb_io_leave_critical(saved);
    return have;
}
bool usb_io_rx_overflowed(void)
{
    uint32_t saved = usb_io_enter_critical();
    bool overflow = s_overflow;
    s_overflow = false;
    usb_io_leave_critical(saved);
    return overflow;
}
bool usb_io_tx_submit(const uint8_t report[64], uint32_t epoch, uint32_t token,
                      uint32_t deadline, bool last, bool reopen)
{
    uint32_t saved = usb_io_enter_critical();
    bool ok = epoch == s_epoch && s_mounted && !s_tx_pending;
    if (ok) {
        memcpy(s_tx.report, report, 64);
        s_tx.epoch = epoch; s_tx.token = token; s_tx.deadline = deadline;
        s_tx.last = last; s_tx.reopen = reopen;
        s_token = token; s_tx_pending = true;
    }
    usb_io_leave_critical(saved);
    return ok;
}
void usb_io_tx_cancel(uint32_t token)
{
    uint32_t saved = usb_io_enter_critical();
    if (s_token == token) { s_token = 0; s_tx_pending = s_done_pending = false; }
    usb_io_leave_critical(saved);
}
void usb_io_report_completed(bool delivered)
{
    uint32_t saved = usb_io_enter_critical();
    uint32_t now = usb_port_millis();
    if (s_in_flight) {
        s_in_flight = false;
        if (s_active.epoch == s_epoch && s_active.token == s_token) {
            delivered = delivered && (int32_t)(now - s_active.deadline) < 0;
            s_done.epoch = s_active.epoch; s_done.token = s_active.token;
            s_done.next_ticket = s_ticket; s_done.delivered = delivered;
            if (delivered && s_active.last && s_active.reopen && s_mounted) {
                ++s_ticket; s_done.next_ticket = s_ticket;
                /* 完成证据和新接收窗口在同一临界区发布。 */
                s_done_pending = true; s_gate = true;
            } else s_done_pending = true;
        }
    }
    usb_io_leave_critical(saved);
}
bool usb_io_done_take(usb_io_done_t *out)
{
    uint32_t saved = usb_io_enter_critical();
    bool have = s_done_pending;
    if (have) { *out = s_done; s_done_pending = false; }
    usb_io_leave_critical(saved);
    return have;
}
uint32_t usb_io_connect_request(bool connected)
{
    uint32_t saved = usb_io_enter_critical();
    uint32_t serial = ++s_connect_serial;
    s_connect_epoch = s_epoch; s_connect_on = connected; s_connect_pending = true;
    usb_io_leave_critical(saved);
    return serial;
}
bool usb_io_connect_applied(uint32_t serial, uint32_t *actual_ms)
{
    uint32_t saved = usb_io_enter_critical();
    bool applied = serial == s_applied_serial;
    if (applied && actual_ms) *actual_ms = s_applied_ms;
    usb_io_leave_critical(saved);
    return applied;
}
void usb_io_task(void)
{
    uint32_t saved, serial, epoch;
    bool connect = false, have_connect = false;
    saved = usb_io_enter_critical();
    if (s_connect_pending) {
        serial = s_connect_serial; epoch = s_connect_epoch; connect = s_connect_on;
        have_connect = epoch == s_epoch;
        if (have_connect) s_connect_pending = false;
        else s_connect_epoch = s_epoch; /* 取消本次旧代次，当前控制意图下轮重投。 */
    } else { serial = 0; epoch = 0; }
    usb_io_leave_critical(saved);
    if (have_connect) {
        if (connect) (void)tud_connect();
        else { (void)tud_disconnect(); usb_io_link_changed(false); }
        saved = usb_io_enter_critical();
        s_applied_serial = serial; s_applied_ms = usb_port_millis();
        usb_io_leave_critical(saved);
    }
    /* RTOS 模式不能无限等待，否则后台状态与看门狗无法推进。 */
    tud_task_ext(0, false);
    saved = usb_io_enter_critical();
    if (s_tx_pending && !s_in_flight) {
        if (s_tx.epoch != s_epoch || s_tx.token != s_token ||
            (int32_t)(usb_port_millis() - s_tx.deadline) >= 0) s_tx_pending = false;
        else if (tud_hid_ready()) {
            s_active = s_tx; s_tx_pending = false; s_in_flight = true;
            /* TinyUSB 仅由本 IO 上下文调用，endpoint mutex 无竞争；短临界区隔离 reset ISR。 */
            if (!tud_hid_report(0, s_active.report, 64)) {
                s_in_flight = false;
                s_tx = s_active; s_tx_pending = true;
            }
        }
    }
    usb_io_leave_critical(saved);
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                           const uint8_t *report, uint16_t len)
{
    if (instance == 0 && id == 0 && type == HID_REPORT_TYPE_OUTPUT)
        usb_io_report_received(report, len);
}
void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t len)
{
    (void)report;
    if (instance == 0 && len == 64) usb_io_report_completed(true);
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                               uint8_t *report, uint16_t len)
{
    (void)instance; (void)id; (void)type; (void)report; (void)len;
    return 0;
}
