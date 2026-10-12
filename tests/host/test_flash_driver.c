#include "semphr.h"
/* 真实 W25Q64/SPI2 驱动：验证非阻塞指令、片选、忙碌门禁与实际超时。 */
#include "w25q64.h"
#include "spi2_bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
SPI_HandleTypeDef hspi2;
static uint32_t ticks, primask, last_addr, ipsr;
static uint8_t command, page[256];
static unsigned transactions, status_reads, enables, page_bytes;
static bool selected, chip_busy, io_failure;
uint32_t HAL_GetTick(void)
{
    return ticks;
}
uint32_t __get_PRIMASK(void)
{
    return primask;
}
void __disable_irq(void)
{
    primask = 1;
}
void __enable_irq(void)
{
    primask = 0;
}
void HAL_GPIO_WritePin(void *port, uint16_t pin, GPIO_PinState state)
{
    (void)port;
    (void)pin;
    CHECK(spi2_bus_owner() == SPI2_BUS_OWNER_FLASH);
    if (state == GPIO_PIN_RESET)
    {
        CHECK(!selected);
        selected = true;
        command = 0;
        transactions++;
    }
    else
    {
        CHECK(selected);
        selected = false;
        if (command == 0x02 || command == 0x20)
            chip_busy = true;
    }
}
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, const uint8_t *p, uint16_t n,
                                   uint32_t timeout)
{
    CHECK(h == &hspi2 && selected && timeout == 100);
    if (io_failure)
    {
        h->State = 1;
        return HAL_ERROR;
    }
    if (!command)
    {
        command = p[0];
        if (command == 0x06)
            enables++;
        if (command == 0x02 || command == 0x20 || command == 0x03)
        {
            CHECK(n == 4);
            last_addr = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }
    }
    else
    {
        CHECK(command == 0x02 && n <= sizeof(page));
        memcpy(page, p, n);
        page_bytes = n;
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t timeout)
{
    CHECK(h == &hspi2 && selected && timeout == 100);
    if (io_failure)
        return HAL_ERROR;
    if (command == 0x05)
    {
        CHECK(n == 1);
        p[0] = chip_busy ? 1 : 0;
        status_reads++;
    }
    else if (command == 0x9F)
    {
        CHECK(n == 3);
        p[0] = 0xEF;
        p[1] = 0x40;
        p[2] = 0x17;
    }
    else
    {
        CHECK(command == 0x03 && !chip_busy);
        memset(p, 0x5A, n);
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *h)
{
    h->State = HAL_SPI_STATE_READY;
    return HAL_OK;
}
int main(int argc, char **argv)
{
    uint8_t data[256];
    unsigned before, statuses;
    CHECK(argc == 2 && spi2_bus_init() && w25q64_init());
    CHECK(g_w25q64_jedec_id == W25Q64_JEDEC_ID);
    CHECK(spi2_bus_try_acquire(SPI2_BUS_OWNER_FLASH));
    ipsr=1;
    CHECK(!spi2_bus_reserve_flash(true) && !spi2_bus_try_acquire(SPI2_BUS_OWNER_MOTOR));
    spi2_bus_release(); CHECK(spi2_bus_owner()==SPI2_BUS_OWNER_FLASH);
    ipsr=0; spi2_bus_release();
    CHECK(spi2_bus_try_acquire(SPI2_BUS_OWNER_MOTOR));
    CHECK(!spi2_bus_reserve_flash(true));
    CHECK(w25q64_erase_start(0x1000) == W25Q64_BUSY);
    spi2_bus_release();
    primask = 1;
    CHECK(spi2_bus_reserve_flash(true) && primask == 1);
    primask = 0;
    CHECK(!spi2_bus_try_acquire(SPI2_BUS_OWNER_MOTOR));
    CHECK(!spi2_bus_try_acquire(SPI2_BUS_OWNER_NONE));
    CHECK(w25q64_erase_start(1) == W25Q64_ERROR);
    CHECK(w25q64_program_start(255, data, 2) == W25Q64_ERROR);
    before = transactions;
    statuses = status_reads;
    CHECK(w25q64_erase_start(0x1000) == W25Q64_OK);
    CHECK(transactions == before + 2 && status_reads == statuses && enables == 1);
    CHECK(last_addr == 0x1000 && !selected && spi2_bus_owner() == SPI2_BUS_OWNER_NONE);
    CHECK(w25q64_read(0, data, sizeof(data)) == W25Q64_BUSY);
    CHECK(w25q64_write(0,data,1)==W25Q64_BUSY && w25q64_erase_sector_4k(0)==W25Q64_BUSY);
    CHECK(w25q64_erase_start(0x2000) == W25Q64_BUSY);
    CHECK(w25q64_poll() == 1 && status_reads == statuses + 1);
    if (!strcmp(argv[1], "erase_timeout"))
    {
        ticks = 999;
        CHECK(w25q64_poll() == 1);
        ticks = 1000;
        CHECK(w25q64_poll() == -1);
    }
    else if (!strcmp(argv[1], "bus_timeout"))
    {
        CHECK(spi2_bus_try_acquire(SPI2_BUS_OWNER_FLASH));
        ticks = 999;
        CHECK(w25q64_poll() == 1);
        ticks = 1000;
        CHECK(w25q64_poll() == 1);
        spi2_bus_release();
        CHECK(w25q64_poll() == -1);
    }
    else if (!strcmp(argv[1], "io_fault"))
    {
        io_failure = true;
        CHECK(w25q64_poll() == -1);
        CHECK(!selected && hspi2.State == HAL_SPI_STATE_READY);
    }
    else
    {
        CHECK(!strcmp(argv[1], "normal"));
        chip_busy = false;
        CHECK(w25q64_poll() == 0);
        CHECK(w25q64_read(0x1000, data, sizeof(data)) == W25Q64_OK && data[0] == 0x5A);
        ticks = 0xFFFFFFF0u;
        CHECK(w25q64_program_start(0x1000, data, sizeof(data)) == W25Q64_OK);
        CHECK(page_bytes == 256 && !memcmp(page, data, 256) && enables == 2);
        ticks += 99;
        CHECK(w25q64_poll() == 1);
        ticks++;
        CHECK(w25q64_poll() == -1); /* 页编程超时跨 tick 回绕。 */
    }
    CHECK(w25q64_read(0, data, 1) == W25Q64_ERROR);
    CHECK(w25q64_program_start(0, data, 1) == W25Q64_ERROR);
    CHECK(spi2_bus_reserve_flash(false));
    CHECK(spi2_bus_try_acquire(SPI2_BUS_OWNER_MOTOR));
    spi2_bus_release();
    CHECK(!selected && !primask);
    printf("flash driver %s passed\n", argv[1]);
    return 0;
}

static uint32_t critical_saved;
uint32_t __get_IPSR(void) { return ipsr; }
void *xTaskGetCurrentTaskHandle(void) { return (void *)1; }
void test_task_enter(void) { critical_saved=primask; primask=1; }
void test_task_exit(void) { primask=critical_saved; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *m) { m->locked=0; m->holder=0; return m; }
int xSemaphoreTake(SemaphoreHandle_t m,uint32_t wait) { (void)wait; if(m->locked)return 0; m->locked=1; m->holder=xTaskGetCurrentTaskHandle(); return 1; }
int xSemaphoreGive(SemaphoreHandle_t m) { CHECK(m->locked);m->locked=0;m->holder=0;return 1; }
void *xSemaphoreGetMutexHolder(SemaphoreHandle_t m) { return m->holder; }
