/**
  ******************************************************************************
  * @file    bsp.h
  * @brief   板级公共初始化：时钟 / 电源 / LED / 按键 / 调试串口
  *
  * 板载资源（自研板 Schematic2：STM32U575OIY6QTR / WLCSP90）
  *   网表：Netlist_Schematic2_2026-09-03-02.tel
  *
  *   LED1~LED4 四颗白光 = PG14 (U1.A9)   ← 灌电流，拉低才亮
  *   LED6 0603          = PD2  (U1.A5)   ← 灌电流，拉低才亮
  *   LED5 是 3V3_ORI 的电源指示灯，硬连（3V3_ORI -> R83 -> LED5 -> GND），MCU 管不着
  *
  *   本板【没有】用户按键：SW1 = NRST（复位），SW2 = CC1310 的 RESET_N
  *   调试口 = H15（2x5 排针，自定义针序，不是标准 Cortex 10 针）：
  *       1=5V  3=3V3  5=GND  7=PA14(SWCLK)  9=PA13(SWDIO)
  *       2=PA9 4=PA10 6=NRST 8=NC           10=NC
  *   串口 = USART1 (PA9=TX / PA10=RX)，挂在 H15.2 / H15.4
  *
  * ⚠ 本板 LED 是【低电平点亮】，和 Nucleo 上那三颗（高电平点亮）正好相反。
  * ⚠ PC13 在本板是摄像头的 PWDN，PC14 是 RESETB，千万不要当按键/普通 IO 用。
  ******************************************************************************
  */
#ifndef __BSP_H
#define __BSP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32u5xx_hal.h"
#include <stdio.h>

/* ---------------- 本板 LED（灌电流接法：拉低才亮） ---------------- */
#define LED_W_PIN               GPIO_PIN_14         /* PG14 -> LED1~LED4 四颗白光 */
#define LED_W_GPIO_PORT         GPIOG
#define LED_W_GPIO_CLK_ENABLE() __HAL_RCC_GPIOG_CLK_ENABLE()

#define LED6_PIN                GPIO_PIN_2          /* PD2  -> R85 -> LED6 (0603) */
#define LED6_GPIO_PORT          GPIOD
#define LED6_GPIO_CLK_ENABLE()  __HAL_RCC_GPIOD_CLK_ENABLE()

/* 亮 / 灭 对应的引脚电平。灌电流接法，所以是反的 */
#define LED_ON_LEVEL            GPIO_PIN_RESET
#define LED_OFF_LEVEL           GPIO_PIN_SET

/* 旧代码里的 LD1/LD2/LD3 名字保留下来，指向本板的灯。
   状态指示统一用 LED6（单颗 0603，比 4 颗白光串在一起好认）。 */
#define LD1_PIN                 LED_W_PIN
#define LD1_GPIO_PORT           LED_W_GPIO_PORT
#define LD1_GPIO_CLK_ENABLE()   LED_W_GPIO_CLK_ENABLE()

#define LD2_PIN                 LED6_PIN
#define LD2_GPIO_PORT           LED6_GPIO_PORT
#define LD2_GPIO_CLK_ENABLE()   LED6_GPIO_CLK_ENABLE()

#define LD3_PIN                 LED6_PIN
#define LD3_GPIO_PORT           LED6_GPIO_PORT
#define LD3_GPIO_CLK_ENABLE()   LED6_GPIO_CLK_ENABLE()

/* ---------------- ST-LINK 虚拟串口 ---------------- */
#define VCP_UART                USART1
#define VCP_UART_CLK_ENABLE()   __HAL_RCC_USART1_CLK_ENABLE()
#define VCP_TX_PIN              GPIO_PIN_9      /* PA9  */
#define VCP_RX_PIN              GPIO_PIN_10     /* PA10 */
#define VCP_GPIO_PORT           GPIOA
#define VCP_GPIO_CLK_ENABLE()   __HAL_RCC_GPIOA_CLK_ENABLE()
#define VCP_GPIO_AF             GPIO_AF7_USART1
/* 串口波特率。默认 921600；串流 demo 用 -DVCP_BAUDRATE=... 覆盖成更高的值 */
#ifndef VCP_BAUDRATE
#define VCP_BAUDRATE            921600U
#endif

extern UART_HandleTypeDef huart1;

/**
  * @brief HAL_Init + 160MHz 时钟 + SMPS/VDDIO2 + ICACHE + 调试串口
  *        每个 app 的 main() 第一句都调它
  */
void BSP_Init(void);

/** @brief 只初始化 LD3 红灯（其余 LED 可能被 DCMI 占用，按需自己初始化） */
void BSP_LedInit(GPIO_TypeDef *port, uint16_t pin);

/** @brief 把一段二进制数据以 UART 原始字节发出去（不经 printf，速度快） */
void BSP_UartWrite(const uint8_t *data, uint32_t len);

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_H */
