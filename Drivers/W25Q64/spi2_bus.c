/* SPI2 任务互斥，维护会话只预留 Flash，不跨会话持有 mutex。 */
#include "spi2_bus.h"
#include "FreeRTOS.h"
#include "semphr.h"
extern SPI_HandleTypeDef hspi2;
static StaticSemaphore_t s_storage;
static SemaphoreHandle_t s_mutex;
static volatile spi2_bus_owner_t s_owner;
static volatile bool s_reserved;
bool spi2_bus_init(void) { s_mutex = xSemaphoreCreateMutexStatic(&s_storage); return s_mutex != 0; }
bool spi2_bus_reserve_flash(bool reserve)
{
    bool ok;
    if (__get_IPSR() != 0) return false;
    taskENTER_CRITICAL();
    ok = !reserve || s_owner != SPI2_BUS_OWNER_MOTOR;
    if (ok) s_reserved = reserve;
    taskEXIT_CRITICAL();
    return ok;
}
bool spi2_bus_try_acquire(spi2_bus_owner_t owner)
{
    bool allowed;
    if (!s_mutex || owner == SPI2_BUS_OWNER_NONE || __get_IPSR() != 0) return false;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    taskENTER_CRITICAL();
    allowed = !(s_reserved && owner == SPI2_BUS_OWNER_MOTOR);
    if (allowed) s_owner = owner;
    taskEXIT_CRITICAL();
    if (!allowed) (void)xSemaphoreGive(s_mutex);
    return allowed;
}
void spi2_bus_release(void)
{
    if (__get_IPSR() != 0) return;
    if (s_mutex && xSemaphoreGetMutexHolder(s_mutex) == xTaskGetCurrentTaskHandle()) {
        taskENTER_CRITICAL(); s_owner = SPI2_BUS_OWNER_NONE; taskEXIT_CRITICAL();
        (void)xSemaphoreGive(s_mutex);
    }
}
spi2_bus_owner_t spi2_bus_owner(void) { return s_owner; }
SPI_HandleTypeDef *spi2_bus_handle(void) { return &hspi2; }
