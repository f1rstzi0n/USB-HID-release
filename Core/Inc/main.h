/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned long ulong;

extern unsigned char MAIN_ucSerialTxBuff[100];
extern unsigned char MAIN_ucSerialRxBuff[100];
extern unsigned char MAIN_ucSerialRxTxFlag;
extern unsigned char MAIN_ucSerialTxCount;
extern unsigned char MAIN_ucSerialRxCount;
extern unsigned char MAIN_ucSerialRxTxSize;

typedef struct {
	ushort baud;
	ushort flags;
	uchar rcvbuf[100];
	uchar rcvbufb[100];
	ushort rcvpos,rcvlen;
	uchar xmtbuf[100];
	ushort xmtpos,xmtlen;
}Uart1Record, *Uart1Ptr;
extern Uart1Record Uart1;

#define STX 0x02
#define ACK 0x06
#define NAK 0x16
#define RX_PENDING 0x0001
#define TX_PENDING 0x0002
#define PACKET_FAULT 0x0100
#define PACKET_TRASH 0x0200
/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define SPI1_CS_DAC_Pin GPIO_PIN_2
#define SPI1_CS_DAC_GPIO_Port GPIOE
#define MOTOR_EN_SIGNAL_IN_Pin GPIO_PIN_3
#define MOTOR_EN_SIGNAL_IN_GPIO_Port GPIOE
#define O_USB_DP_CTRL_Pin GPIO_PIN_4
#define O_USB_DP_CTRL_GPIO_Port GPIOE
#define LED1_Pin GPIO_PIN_5
#define LED1_GPIO_Port GPIOE
#define PFO_O_Pin GPIO_PIN_6
#define PFO_O_GPIO_Port GPIOE
#define I_LOCK_POWER_OPEN_Pin GPIO_PIN_13
#define I_LOCK_POWER_OPEN_GPIO_Port GPIOC
#define I_LOCK_POWER_CLOSE_Pin GPIO_PIN_14
#define I_LOCK_POWER_CLOSE_GPIO_Port GPIOC
#define I_LOCK_INTERFACE_CLOSE_Pin GPIO_PIN_15
#define I_LOCK_INTERFACE_CLOSE_GPIO_Port GPIOC
#define O_LOCK_INTERFACE_OPEN_Pin GPIO_PIN_0
#define O_LOCK_INTERFACE_OPEN_GPIO_Port GPIOC
#define DAC_LDAC_Pin GPIO_PIN_1
#define DAC_LDAC_GPIO_Port GPIOC
#define SPI2_MISO_FLASH_MOTOR_Pin GPIO_PIN_2
#define SPI2_MISO_FLASH_MOTOR_GPIO_Port GPIOC
#define SPI2_MOSI_FLASH_MOTOR_Pin GPIO_PIN_3
#define SPI2_MOSI_FLASH_MOTOR_GPIO_Port GPIOC
#define KEY_Pin GPIO_PIN_0
#define KEY_GPIO_Port GPIOA
#define ADC1IN1_CAP_Pin GPIO_PIN_1
#define ADC1IN1_CAP_GPIO_Port GPIOA
#define ADC1IN2_48V_Pin GPIO_PIN_2
#define ADC1IN2_48V_GPIO_Port GPIOA
#define ADC1IN3_24V_Pin GPIO_PIN_3
#define ADC1IN3_24V_GPIO_Port GPIOA
#define ADC1IN4_5V_Pin GPIO_PIN_4
#define ADC1IN4_5V_GPIO_Port GPIOA
#define ADC1IN5_24V_Pin GPIO_PIN_5
#define ADC1IN5_24V_GPIO_Port GPIOA
#define ADC1IN6_VOUT_Pin GPIO_PIN_6
#define ADC1IN6_VOUT_GPIO_Port GPIOA
#define ADC1IN7_CDA_Pin GPIO_PIN_7
#define ADC1IN7_CDA_GPIO_Port GPIOA
#define DISPLAY_STCP_Pin GPIO_PIN_4
#define DISPLAY_STCP_GPIO_Port GPIOC
#define DISPLAY_SHCP_Pin GPIO_PIN_5
#define DISPLAY_SHCP_GPIO_Port GPIOC
#define CONTROL_Pin GPIO_PIN_0
#define CONTROL_GPIO_Port GPIOB
#define CLKS_Pin GPIO_PIN_1
#define CLKS_GPIO_Port GPIOB
#define DATAIN_Pin GPIO_PIN_2
#define DATAIN_GPIO_Port GPIOB
#define DATAOUT_Pin GPIO_PIN_7
#define DATAOUT_GPIO_Port GPIOE
#define SPI2_CS_FLASH_Pin GPIO_PIN_8
#define SPI2_CS_FLASH_GPIO_Port GPIOE
#define T1_CH1_ADMCLK_Pin GPIO_PIN_9
#define T1_CH1_ADMCLK_GPIO_Port GPIOE
#define LED2_Pin GPIO_PIN_10
#define LED2_GPIO_Port GPIOE
#define SPI2_CS_MOTOR_Pin GPIO_PIN_11
#define SPI2_CS_MOTOR_GPIO_Port GPIOE
#define SPI4_SCK__EEPROM_Pin GPIO_PIN_12
#define SPI4_SCK__EEPROM_GPIO_Port GPIOE
#define SPI4_MISO_EEPROM_Pin GPIO_PIN_13
#define SPI4_MISO_EEPROM_GPIO_Port GPIOE
#define SPI4_MOSI_EEPROM_Pin GPIO_PIN_14
#define SPI4_MOSI_EEPROM_GPIO_Port GPIOE
#define PFO_I_Pin GPIO_PIN_15
#define PFO_I_GPIO_Port GPIOE
#define SPI4_CS_EEPROM_Pin GPIO_PIN_10
#define SPI4_CS_EEPROM_GPIO_Port GPIOB
#define I_DEVICETYPE_3_Pin GPIO_PIN_12
#define I_DEVICETYPE_3_GPIO_Port GPIOB
#define I_DEVICETYPE_2_Pin GPIO_PIN_13
#define I_DEVICETYPE_2_GPIO_Port GPIOB
#define ARISWITCH_Pin GPIO_PIN_15
#define ARISWITCH_GPIO_Port GPIOB
#define O_FAN_Pin GPIO_PIN_10
#define O_FAN_GPIO_Port GPIOD
#define I_DEVICETYPE_1_Pin GPIO_PIN_11
#define I_DEVICETYPE_1_GPIO_Port GPIOD
#define DISPLAY_DS_Pin GPIO_PIN_12
#define DISPLAY_DS_GPIO_Port GPIOD
#define LED3_Pin GPIO_PIN_13
#define LED3_GPIO_Port GPIOD
#define LED4_Pin GPIO_PIN_14
#define LED4_GPIO_Port GPIOD
#define USART1_CTRL_Pin GPIO_PIN_15
#define USART1_CTRL_GPIO_Port GPIOD
#define AD_READY_INT_Pin GPIO_PIN_6
#define AD_READY_INT_GPIO_Port GPIOC
#define O_LOCK_POWER_OPEN_Pin GPIO_PIN_7
#define O_LOCK_POWER_OPEN_GPIO_Port GPIOC
#define I_LOCK_INTERFACE_OPEN_Pin GPIO_PIN_8
#define I_LOCK_INTERFACE_OPEN_GPIO_Port GPIOC
#define SPI3_CS_ADC_Pin GPIO_PIN_10
#define SPI3_CS_ADC_GPIO_Port GPIOA
#define LOCK_POWER_CLOSE_Pin GPIO_PIN_15
#define LOCK_POWER_CLOSE_GPIO_Port GPIOA
#define SPI3_SCK_ADC_Pin GPIO_PIN_10
#define SPI3_SCK_ADC_GPIO_Port GPIOC
#define SPI3_MISO_ADC_Pin GPIO_PIN_11
#define SPI3_MISO_ADC_GPIO_Port GPIOC
#define SPI3_MOSI_ADC_Pin GPIO_PIN_12
#define SPI3_MOSI_ADC_GPIO_Port GPIOC
#define O_LOCK_INTERFACE_CLOSE_Pin GPIO_PIN_2
#define O_LOCK_INTERFACE_CLOSE_GPIO_Port GPIOD
#define SPI2_SCK__FLASH_MOTOR_Pin GPIO_PIN_3
#define SPI2_SCK__FLASH_MOTOR_GPIO_Port GPIOD
#define USART2_CTRL_Pin GPIO_PIN_4
#define USART2_CTRL_GPIO_Port GPIOD
#define SPI1_CS_ECAT_Pin GPIO_PIN_7
#define SPI1_CS_ECAT_GPIO_Port GPIOD
#define SPI1_MISO_ECAT_DAC_Pin GPIO_PIN_3
#define SPI1_MISO_ECAT_DAC_GPIO_Port GPIOB
#define SPI1_MISO_ECAT_DACB4_Pin GPIO_PIN_4
#define SPI1_MISO_ECAT_DACB4_GPIO_Port GPIOB
#define SPI1_MOSI_ECAT_DAC_Pin GPIO_PIN_5
#define SPI1_MOSI_ECAT_DAC_GPIO_Port GPIOB
#define SYSTEM_LED_Pin GPIO_PIN_8
#define SYSTEM_LED_GPIO_Port GPIOB
#define ECAT_SYNCO_INT_Pin GPIO_PIN_9
#define ECAT_SYNCO_INT_GPIO_Port GPIOB
#define INT_ESC_Pin GPIO_PIN_0
#define INT_ESC_GPIO_Port GPIOE
#define EEPROM_Loaded_Pin GPIO_PIN_1
#define EEPROM_Loaded_GPIO_Port GPIOE

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
