/* SPI2 总线互斥仲裁。
 *
 * 硬件事实：W25Q64（FLASH_CS# = PE8）与电机接口（MOTOR_CS# = PE11）共享同一条
 * SPI2（PD3=SCK、PC2=MISO、PC3=MOSI），因此**任何 SPI2 事务都必须包在
 * acquire/release 之间**，否则一次电机事务会被 Flash 读盘插进来撕成两半。
 *
 * 给电机侧（尚未入库）的契约：
 *   1. 发起事务前 `spi2_bus_try_acquire(SPI2_BUS_OWNER_MOTOR)`；返回 false 表示
 *      总线被占，**放弃本次事务、下一轮重来**，不要在这里自旋等待；
 *   2. 事务结束（含把自己的 CS 拉高）后立刻 `spi2_bus_release()`；
 *   3. 持有总线期间不得调用可能长时间阻塞的操作。运行期 Flash 逐条指令释放锁，
 *      擦写等待由主循环轮询；整个更新会话另行预留总线，电机侧应跳过本次事务。
 *
 * 为什么不做阻塞排队：两侧都只需要"拿不到就下次再来"的语义，阻塞等待反而引入
 * 优先级反转（电机侧若在中断上下文等主循环持有的锁，会直接死锁）。 */
#ifndef SPI2_BUS_H
#define SPI2_BUS_H

#include <stdbool.h>

#include "stm32f4xx_hal.h"

/* 总线占用者。用于调试观测（J-Link 看 spi2_bus_owner()）与死锁归因。 */
typedef enum {
    SPI2_BUS_OWNER_NONE  = 0u,
    SPI2_BUS_OWNER_FLASH = 1u,   /* W25Q64（firmware/w25q64.c） */
    SPI2_BUS_OWNER_MOTOR = 2u    /* 电机接口（MOTOR_CS# = PE11，尚未入库） */
} spi2_bus_owner_t;

/* 非阻塞取总线：成功返回 true（此后独占至 release），已被占返回 false。 */
bool spi2_bus_init(void);
bool spi2_bus_try_acquire(spi2_bus_owner_t owner);
/* 更新会话预留：只阻止 MOTOR，Flash 仍按事务加锁。 */
bool spi2_bus_reserve_flash(bool reserve);

/* 释放总线。只应由当前持有者调用；未持有时调用是无害的空操作。 */
void spi2_bus_release(void);

/* 当前占用者（SPI2_BUS_OWNER_NONE 表示空闲）。纯观测用。 */
spi2_bus_owner_t spi2_bus_owner(void);

/* SPI2 句柄（CubeMX 生成于 Core/Src/main.c 的 hspi2）。
 * 集中在这里暴露，免得每个使用者各写一遍 extern。 */
SPI_HandleTypeDef *spi2_bus_handle(void);

#endif /* SPI2_BUS_H */
