/**
  ******************************************************************************
  * @file    app_led.c
  * @brief   STM32U575OIY6QTR 自研板 LED6 快速闪烁
  *
  * 网表 Netlist_Schematic2_2026-09-03-02.tel：
  *   PD2 (U1.A5) -> R85 -> LED6.1
  *   3V3         -> LED6.2
  *
  * LED6 采用灌电流接法，因此 PD2 拉低时点亮、拉高时熄灭。
  ******************************************************************************
  */

#include "bsp.h"

/* 亮 100 ms、灭 100 ms：完整周期 200 ms，即 5 Hz。 */
#define LED6_HALF_PERIOD_MS       100U
#define LED_W_HALF_PERIOD_MS      500U

static void led6_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  LED6_GPIO_CLK_ENABLE();

  /* 先预置为熄灭状态，再切换为输出，避免初始化瞬间误亮。 */
  HAL_GPIO_WritePin(LED6_GPIO_PORT, LED6_PIN, LED_OFF_LEVEL);

  gpio.Pin   = LED6_PIN;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED6_GPIO_PORT, &gpio);
}

static void white_leds_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  LED_W_GPIO_CLK_ENABLE();
  HAL_GPIO_WritePin(LED_W_GPIO_PORT, LED_W_PIN, LED_OFF_LEVEL);

  gpio.Pin   = LED_W_PIN;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_W_GPIO_PORT, &gpio);
}

int main(void)
{
  BSP_Init();
  led6_init();
  white_leds_init();

  printf("\r\nSTM32U575 LED6 快闪测试\r\n");
  printf("PD2 (U1.A5)，低电平点亮，亮/灭各 %lu ms（5 Hz）\r\n",
         (unsigned long)LED6_HALF_PERIOD_MS);

  while (1)
  {
    static uint32_t last_led6;
    static uint32_t last_white;
    uint32_t now = HAL_GetTick();

    if ((uint32_t)(now - last_led6) >= LED6_HALF_PERIOD_MS)
    {
      last_led6 = now;
      HAL_GPIO_TogglePin(LED6_GPIO_PORT, LED6_PIN);
    }

    if ((uint32_t)(now - last_white) >= LED_W_HALF_PERIOD_MS)
    {
      last_white = now;
      HAL_GPIO_TogglePin(LED_W_GPIO_PORT, LED_W_PIN);
    }

    HAL_Delay(1);
  }
}
