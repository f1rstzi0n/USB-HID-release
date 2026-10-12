/* 主机测试只替换 HAL 边界，驱动与总线仲裁使用固件原文件。 */
#ifndef FLASH_TEST_HAL_H
#define FLASH_TEST_HAL_H
#include <stdint.h>
typedef struct
{
    unsigned State;
} SPI_HandleTypeDef;
typedef enum
{
    HAL_OK,
    HAL_ERROR
} HAL_StatusTypeDef;
typedef enum
{
    GPIO_PIN_RESET,
    GPIO_PIN_SET
} GPIO_PinState;
#define HAL_SPI_STATE_READY 0u
uint32_t HAL_GetTick(void);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __enable_irq(void);
void HAL_GPIO_WritePin(void *, uint16_t, GPIO_PinState);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *, const uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *);
uint32_t __get_IPSR(void);
#endif
