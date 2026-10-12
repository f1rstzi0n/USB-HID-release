/* 实际媒体 adapter：锁忙不访问 Flash，解锁后发布一致容量。 */
#include "usb_media.h"
#include "semphr.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static bool ready;
static uint32_t blocks,reads;
static void *task=(void *)1;
uint32_t usb_io_enter_critical(void) { return 0; }
void usb_io_leave_critical(uint32_t saved) { (void)saved; }
bool content_store_ready(void) { return ready; }
uint32_t content_store_block_count(void) { return blocks; }
uint8_t content_store_media_event(void) { return 2; }
bvpv_cd_read_result_t content_store_read(uint32_t off,uint8_t *out,uint32_t len)
{ (void)off;++reads;memset(out,0xA5,len);return BVPV_CD_READ_OK; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *m)
{ m->locked=0;m->holder=0;return m; }
int xSemaphoreTake(SemaphoreHandle_t m,uint32_t wait)
{ (void)wait;if(m->locked)return 0;m->locked=1;m->holder=task;return 1; }
int xSemaphoreGive(SemaphoreHandle_t m)
{ assert(m->locked&&m->holder==task);m->locked=0;m->holder=0;return 1; }
int main(void)
{
    uint8_t buf[64];
    bool snapshot;
    assert(usb_media_init());usb_media_lock();ready=true;blocks=123;
    task=(void *)2;
    assert(usb_media_read(0,buf,64)==BVPV_CD_READ_BUSY && !reads);
    assert(!usb_media_event(&snapshot) && !usb_media_ready() && !usb_media_block_count());
    task=(void *)1;usb_media_unlock();
    assert(usb_media_ready() && usb_media_block_count()==123);
    task=(void *)2;
    assert(usb_media_read(0,buf,64)==BVPV_CD_READ_OK && reads==1 && buf[0]==0xA5);
    assert(usb_media_event(&snapshot)==2);
    task=(void *)1;usb_media_lock();ready=false;blocks=0;usb_media_unlock();
    assert(!usb_media_ready() && !usb_media_block_count());
    puts("USB media BUSY and snapshot passed");return 0;
}
