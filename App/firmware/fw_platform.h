#ifndef FW_PLATFORM_H
#define FW_PLATFORM_H
#include <stdbool.h>
typedef bool (*fw_maintenance_check_t)(void);
void fw_platform_app_init(fw_maintenance_check_t);
void fw_platform_task(void);
#endif
