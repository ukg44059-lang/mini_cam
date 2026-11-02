/**
  ******************************************************************************
  * @file    board.h
  * @brief   LAUNCHXL-CC1310 板级引脚定义
  *
  * 板子：TI LAUNCHXL-CC1310 Rev A（板载 CC1310F128RGZ + XDS110 调试器）
  *
  * 引脚号来自 TI 官方板级头 CC1310_LAUNCHXL.h，核实过：
  *
  *   红 LED      DIO6    高电平点亮
  *   绿 LED      DIO7    高电平点亮
  *   BTN-1       DIO13   按下为低（板上有上拉）
  *   BTN-2       DIO14   按下为低
  *   UART TX     DIO3  ┐ 接到板载 XDS110 的 "Application/User UART"，
  *   UART RX     DIO2  ┘ 在电脑上就是那个 COM 口（本机是 COM18）
  *
  * ⚠ 别和自制板搞混：`CC1310F128RSMR_最小系统_XDS110_SPI_RF连接表.md` 里
  *   DIO6/DIO7 是接 STM32 的 SPI MOSI/MISO，那是另一块板子的定义。
  *   LaunchPad 上这两个脚是 LED。
  ******************************************************************************
  */
#ifndef __BOARD_H
#define __BOARD_H

#include <driverlib/ioc.h>

/* ---------------- LED ---------------- */
#define BOARD_LED_RED       IOID_6
#define BOARD_LED_GREEN     IOID_7

/* ---------------- 按键 ---------------- */
#define BOARD_BTN1          IOID_13
#define BOARD_BTN2          IOID_14

/* ---------------- 串口（接 XDS110 的 VCP） ---------------- */
#define BOARD_UART_RX       IOID_2
#define BOARD_UART_TX       IOID_3

#endif /* __BOARD_H */
