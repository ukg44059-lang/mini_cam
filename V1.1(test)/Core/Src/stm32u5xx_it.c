/**
  ******************************************************************************
  * @file    stm32u5xx_it.c
  * @brief   Cortex-M33 内核异常处理
  ******************************************************************************
  */
#include "bsp.h"
#include "stm32u5xx_it.h"

/******************************************************************************/
/*            Cortex-M33 处理器异常                                            */
/******************************************************************************/

void NMI_Handler(void)
{
  while (1)
  {
  }
}

void HardFault_Handler(void)
{
  while (1)
  {
  }
}

void MemManage_Handler(void)
{
  while (1)
  {
  }
}

void BusFault_Handler(void)
{
  while (1)
  {
  }
}

void UsageFault_Handler(void)
{
  while (1)
  {
  }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

/* HAL_Delay / HAL_GetTick 的心跳，1ms 一次 */
void SysTick_Handler(void)
{
  HAL_IncTick();
  extern void Test_LedTick(void) __attribute__((weak));
  if (Test_LedTick) Test_LedTick();
}

/* 守卫用 USE_USB_CDC 而不是 HAL_PCD_MODULE_ENABLED：
   后者在 hal_conf.h 里是常开的，但 USB 的那几个 .c 只有
   APP=stream 且 TRANSPORT=usb 时才进编译。用 HAL_PCD_MODULE_ENABLED
   守卫的话，换个 app 编译就会报 undefined reference to HAL_PCD_IRQHandler。
   USE_USB_CDC 由 Makefile 在加 USB_SOURCES 的同时定义，两者严格同步。 */
#ifdef USE_USB_CDC
extern PCD_HandleTypeDef hpcd_USB_FS;

/**
  * @brief USB OTG_FS 全局中断
  * @note  USB 的枚举、端点收发全靠它。忘了加这个的话，
  *        插上电脑什么反应都没有，跟没接线一模一样。
  */
void OTG_FS_IRQHandler(void)
{
  HAL_PCD_IRQHandler(&hpcd_USB_FS);
}
#endif /* USE_USB_CDC */
