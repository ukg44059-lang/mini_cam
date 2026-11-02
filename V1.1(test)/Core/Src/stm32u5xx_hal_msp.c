/**
  ******************************************************************************
  * @file    stm32u5xx_hal_msp.c
  * @brief   外设底层（时钟 / 引脚 / 中断）初始化
  ******************************************************************************
  */
#include "bsp.h"

/**
  * @brief 全局 MSP 初始化，HAL_Init() 里被调用
  */
void HAL_MspInit(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_RCC_SYSCFG_CLK_ENABLE();
}

/**
  * @brief UART 底层初始化：开时钟 + 配引脚复用
  */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (huart->Instance == VCP_UART)
  {
    VCP_UART_CLK_ENABLE();
    VCP_GPIO_CLK_ENABLE();

    /* PA9 = USART1_TX, PA10 = USART1_RX */
    GPIO_InitStruct.Pin       = VCP_TX_PIN | VCP_RX_PIN;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = VCP_GPIO_AF;
    HAL_GPIO_Init(VCP_GPIO_PORT, &GPIO_InitStruct);
  }
}

/**
  * @brief UART 反初始化
  */
void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
  if (huart->Instance == VCP_UART)
  {
    __HAL_RCC_USART1_FORCE_RESET();
    __HAL_RCC_USART1_RELEASE_RESET();
    HAL_GPIO_DeInit(VCP_GPIO_PORT, VCP_TX_PIN | VCP_RX_PIN);
  }
}
