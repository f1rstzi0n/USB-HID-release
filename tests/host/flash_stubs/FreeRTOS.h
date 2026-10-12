#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
#include <assert.h>
typedef struct { unsigned locked; void *holder; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define configASSERT(x) assert(x)
#define taskENTER_CRITICAL() test_task_enter()
#define taskEXIT_CRITICAL() test_task_exit()
void test_task_enter(void);
void test_task_exit(void);
void *xTaskGetCurrentTaskHandle(void);
#endif
