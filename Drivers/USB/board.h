#ifndef BOARD_H
#define BOARD_H
#include <stdbool.h>
#include <stdint.h>
bool board_usb_hw_init(void);
const char *board_usb_serial(void);
void board_watchdog_refresh(void);
#endif
