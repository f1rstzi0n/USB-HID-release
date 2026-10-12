/* 真实 worker/IO 源码，替换端点边界；确定性注入复位与溢出。 */
#include "usb_io.h"
#include "usb_proto.h"
#include "bvpv_usb_frag.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t ticks,critical,leaves,inject_at;
static unsigned inject_mode,dispatches,successes,failures,sends,connects,disconnects,parts;
static bool endpoint_ready;
static uint8_t policy,received[70][64],sent[64],last_value;
static uint16_t last_len;
static unsigned background;
static void background_step(void) { ++background; }
uint32_t usb_port_millis(void) { return ticks; }
uint32_t usb_io_enter_critical(void) { uint32_t saved=critical;critical=1;return saved; }
void usb_io_leave_critical(uint32_t saved)
{
    unsigned i,mode;
    critical=saved;
    ++leaves;
    if(inject_at && leaves==inject_at) {
        mode=inject_mode;inject_at=0;
        if(mode==1) usb_io_link_reset();
        else for(i=0;i<17;i++) usb_io_report_received(received[0],64);
    }
}
bool tud_connect(void) { ++connects;return true; }
bool tud_disconnect(void) { ++disconnects;endpoint_ready=true;return true; }
void tud_task_ext(uint32_t timeout,bool isr) { assert(!timeout&&!isr); }
bool tud_hid_ready(void) { return endpoint_ready; }
bool tud_hid_report(uint8_t id,const void *report,uint16_t len)
{
    assert(critical && endpoint_ready && !id && len==64);
    memcpy(sent,report,64);endpoint_ready=false;++sends;return true;
}
static size_t dispatch(const bvpv_frame_t *r,uint8_t *out,uint16_t cap)
{
    assert(cap==4096);++dispatches;last_len=r->plen;
    last_value=r->plen?r->payload[0]:0;
    return bvpv_frame_encode(out,r->svc,BVPV_FLAGS_DIR_RESPONSE|BVPV_PROTO_VERSION,r->tid,r->payload,r->plen);
}
static void complete(uint8_t svc,bool delivered) { (void)svc;if(delivered)++successes;else++failures; }
static uint8_t reply_policy(const uint8_t *r,size_t len) { (void)r;(void)len;return policy; }
static const usb_proto_backend_t backend={.task=background_step,.dispatch=dispatch,.reply_complete=complete,.reply_policy=reply_policy};
static void collect(const uint8_t *report,void *ctx) { (void)ctx;assert(parts<70);memcpy(received[parts++],report,64); }
static void request(uint16_t length,uint8_t value)
{
    uint8_t frame[4096],payload[4088];
    bvpv_frag_tx_t tx;
    size_t size;
    memset(payload,value,sizeof(payload));bvpv_frag_tx_init(&tx);parts=0;
    size=bvpv_frame_encode(frame,2,BVPV_PROTO_VERSION,123,payload,length);
    assert(size && bvpv_frag_tx_send(&tx,frame,(uint16_t)size,collect,0));
}
static void setup(void)
{
    ticks=0;critical=leaves=inject_at=0;inject_mode=0;
    dispatches=successes=failures=sends=connects=disconnects=0;
    policy=0;endpoint_ready=true;background=0;
    usb_io_init();usb_proto_init_backend(&backend);usb_io_link_changed(true);usb_proto_task();
}
static void actual_complete(void) { assert(!endpoint_ready);endpoint_ready=true;usb_io_report_completed(true); }
static void finish(void)
{
    unsigned i;
    for(i=0;i<80 && !successes;i++) { usb_io_task();if(!endpoint_ready)actual_complete();usb_proto_task();++ticks; }
    assert(successes==1);
}
static void copied_and_busy(void)
{
    uint8_t copy[64];
    setup();request(1,0x5A);memcpy(copy,received[0],64);
    usb_io_report_received(copy,64);memset(copy,0,64);
    usb_io_report_received(received[0],64);usb_proto_task();
    assert(dispatches==1 && last_value==0x5A && successes==0);
    usb_io_report_received(received[0],64);usb_proto_task();assert(dispatches==1);
    usb_io_task();assert(sends==1 && successes==0);actual_complete();
    /* 实际完成后立即 OUT，worker 必须先消费完成再处理新 ticket。 */
    usb_io_report_received(received[0],64);usb_proto_task();
    assert(successes==1 && dispatches==2);
}
static void overflow_and_reset(void)
{
    unsigned i,mode;
    usb_io_rx_t rx;
    setup();request(100,0x31);usb_io_report_received(received[0],64);usb_proto_task();
    for(i=0;i<17;i++)usb_io_report_received(received[1],64);
    usb_proto_task();assert(dispatches==0 && g_usb_rx_overflows==1);
    usb_io_report_received(received[1],64);usb_proto_task();assert(dispatches==0);
    for(i=0;i<parts;i++)usb_io_report_received(received[i],64);
    usb_proto_task();assert(dispatches==1);finish();
    setup();request(1,0x44);usb_io_report_received(received[0],64);
    assert(usb_io_rx_take(&rx));usb_io_link_reset();
    assert(!usb_io_rx_close(rx.epoch,rx.ticket));usb_proto_task();assert(!dispatches);
    for(mode=1;mode<=2;mode++) {
        setup();request(1,0x44);usb_io_report_received(received[0],64);
        leaves=0;inject_at=5;inject_mode=mode;usb_proto_task();assert(!dispatches);
    }
}
static void maximum_and_late(void)
{
    unsigned i,first_sends;
    setup();request(4088,0x66);
    for(i=0;i<parts;i++) { usb_io_report_received(received[i],64);if(i%8==7)usb_proto_task(); }
    usb_proto_task();assert(dispatches==1 && last_len==4088);finish();assert(sends==68);
    setup();request(1,0x77);usb_io_report_received(received[0],64);usb_proto_task();usb_io_task();
    ticks=501;usb_proto_task();assert(failures==1 && !successes);
    actual_complete();usb_proto_task();assert(!successes);
    usb_io_report_received(received[0],64);usb_proto_task();assert(dispatches==2);
    first_sends=sends;finish();assert(sends==first_sends+1);
    /* 发送超时和迟到完成均不得授权重枚举。 */
    setup();policy=USB_REPLY_REENUMERATE;request(1,0x78);
    usb_io_report_received(received[0],64);usb_proto_task();usb_io_task();
    ticks=501;usb_proto_task();ticks=1000;usb_proto_task();usb_io_task();
    assert(failures==1 && !successes && !disconnects && !connects);
    actual_complete();ticks=2000;usb_proto_task();usb_io_task();
    assert(!successes && !disconnects && !connects);
    setup();policy=USB_REPLY_KEEP_RX_CLOSED;request(1,0x55);
    usb_io_report_received(received[0],64);usb_proto_task();finish();
    usb_io_report_received(received[0],64);usb_proto_task();assert(dispatches==1);
}
static void connect_retry(void)
{
    uint32_t serial,at;
    setup();serial=usb_io_connect_request(true);usb_io_link_reset();usb_io_task();usb_io_task();
    assert(connects==1 && usb_io_connect_applied(serial,&at));
    setup();serial=usb_io_connect_request(false);usb_io_link_reset();usb_io_task();
    assert(!usb_io_connect_applied(serial,&at));usb_io_task();
    assert(usb_io_connect_applied(serial,&at) && disconnects==1);
    setup();policy=USB_REPLY_REENUMERATE;request(1,0x22);
    usb_io_report_received(received[0],64);usb_proto_task();finish();
    ticks+=100;usb_proto_task();usb_io_link_reset();usb_io_task();usb_proto_task();usb_io_task();
    assert(disconnects==1);ticks+=250;usb_proto_task();usb_io_task();assert(connects==1);
    usb_io_link_changed(true);usb_proto_task();policy=0;
    usb_io_report_received(received[0],64);usb_proto_task();assert(dispatches==2);
}
static void reset_completion_isolation(void)
{
    unsigned before;
    setup();request(1,0x11);usb_io_report_received(received[0],64);usb_proto_task();usb_io_task();
    usb_io_link_reset();actual_complete();usb_proto_task();assert(!successes && failures==1);
    before=background;usb_proto_task();assert(background==before+1);
    usb_io_link_changed(true);usb_proto_task();
    usb_io_report_received(received[0],64);usb_proto_task();finish();assert(dispatches==2 && successes==1);
    setup();request(1,0x12);usb_io_report_received(received[0],64);usb_proto_task();usb_io_task();actual_complete();
    usb_io_link_reset();usb_proto_task();assert(!successes && failures==1);
}
int main(void)
{
    copied_and_busy();overflow_and_reset();maximum_and_late();connect_retry();reset_completion_isolation();
    puts("USB handoff: copy, overflow, busy discard, immediate next, maximum, timeout, reset and connect retry passed");
    return 0;
}
