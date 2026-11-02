/**
  ******************************************************************************
  * @file    app_pwm.c
  * @brief   Camera connector continuity test using software-generated PWM.
  *
  * The order is ascending FPC pin number.  All test lines are open-drain with
  * an internal pull-up.  This keeps the MCU from actively driving 3.3 V into
  * the camera's 2.8 V I/O domain and is also safer for camera output lines.
  *
  * IMPORTANT:
  *   Disconnect the camera module before running this test.  PCLK, VSYNC,
  *   HREF and D[7:0] are normally driven by the camera.  This firmware drives
  *   them deliberately for continuity probing at the far end of the cable.
  ******************************************************************************
  */

#include "bsp.h"

typedef struct
{
  const char   *signal;
  const char   *port_name;
  const char   *mcu_pin;
  const char   *fpc_pin;
  GPIO_TypeDef *port;
  uint16_t      pin;
  uint32_t      frequency_hz;
  uint32_t      half_period_cycles;
  uint32_t      next_toggle;
  uint8_t       level_low;
} pwm_line_t;

/*
 * FPC order is intentional: it is the order used in the frequency table
 * printed on the UART and in test_pwm/README.md.
 *
 * FPC 4 = AVDD path, FPC 10 = 1V5, FPC 11 = 2V8, FPC 2/15 = GND;
 * these are deliberately excluded from the PWM list.
 */
static pwm_line_t s_lines[] = {
  { "SDA   ", "GPIOC", "PC1 ", " 3", GPIOC, GPIO_PIN_1,  1000U, 0U, 0U, 1U },
  { "SCL   ", "GPIOC", "PC0 ", " 5", GPIOC, GPIO_PIN_0,  1500U, 0U, 0U, 1U },
  { "RESETB", "GPIOA", "PA2 ", " 6", GPIOA, GPIO_PIN_2,  2000U, 0U, 0U, 1U },
  { "VSYNC ", "GPIOB", "PB7 ", " 7", GPIOB, GPIO_PIN_7,  2500U, 0U, 0U, 1U },
  { "PWDN  ", "GPIOA", "PA1 ", " 8", GPIOA, GPIO_PIN_1,  3000U, 0U, 0U, 1U },
  { "HREF  ", "GPIOA", "PA4 ", " 9", GPIOA, GPIO_PIN_4,  3500U, 0U, 0U, 1U },
  { "D7/Y9 ", "GPIOE", "PE6 ", "12", GPIOE, GPIO_PIN_6,  4000U, 0U, 0U, 1U },
  { "XCLK  ", "GPIOA", "PA8 ", "13", GPIOA, GPIO_PIN_8,  4500U, 0U, 0U, 1U },
  { "D6/Y8 ", "GPIOB", "PB8 ", "14", GPIOB, GPIO_PIN_8,  5000U, 0U, 0U, 1U },
  { "D5/Y7 ", "GPIOB", "PB6 ", "16", GPIOB, GPIO_PIN_6,  5500U, 0U, 0U, 1U },
  { "PCLK  ", "GPIOA", "PA6 ", "17", GPIOA, GPIO_PIN_6,  6000U, 0U, 0U, 1U },
  { "D4/Y6 ", "GPIOE", "PE4 ", "18", GPIOE, GPIO_PIN_4,  6500U, 0U, 0U, 1U },
  { "D0/Y2 ", "GPIOC", "PC6 ", "19", GPIOC, GPIO_PIN_6,  7000U, 0U, 0U, 1U },
  { "D3/Y5 ", "GPIOC", "PC9 ", "20", GPIOC, GPIO_PIN_9,  7500U, 0U, 0U, 1U },
  { "D1/Y3 ", "GPIOC", "PC7 ", "21", GPIOC, GPIO_PIN_7,  8000U, 0U, 0U, 1U },
  { "D2/Y4 ", "GPIOC", "PC8 ", "22", GPIOC, GPIO_PIN_8,  8500U, 0U, 0U, 1U },
};

#define PWM_LINE_COUNT (sizeof(s_lines) / sizeof(s_lines[0]))
#define LED_HALF_PERIOD_MS 500U

static void enable_line_clocks(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
}

static void pwm_gpio_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  enable_line_clocks();

  gpio.Mode  = GPIO_MODE_OUTPUT_OD;
  gpio.Pull  = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

  for (uint32_t i = 0U; i < PWM_LINE_COUNT; i++)
  {
    /* Start low, then switch the pin to open-drain output. */
    s_lines[i].port->BSRR = (uint32_t)s_lines[i].pin << 16U;
    gpio.Pin = s_lines[i].pin;
    HAL_GPIO_Init(s_lines[i].port, &gpio);
  }
}

static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void print_table(void)
{
  printf("\r\nFPC pin  Signal  MCU pin  Frequency\r\n");
  printf("-------  ------  -------  ---------\r\n");

  for (uint32_t i = 0U; i < PWM_LINE_COUNT; i++)
  {
    printf("%7s  %-6s  %-7s  %lu Hz\r\n",
           s_lines[i].fpc_pin,
           s_lines[i].signal,
           s_lines[i].mcu_pin,
           (unsigned long)s_lines[i].frequency_hz);
  }
}

static void pwm_prepare(void)
{
  const uint32_t cpu_hz = HAL_RCC_GetHCLKFreq();

  for (uint32_t i = 0U; i < PWM_LINE_COUNT; i++)
  {
    s_lines[i].half_period_cycles = cpu_hz / (2U * s_lines[i].frequency_hz);
    if (s_lines[i].half_period_cycles == 0U)
    {
      s_lines[i].half_period_cycles = 1U;
    }
  }
}

static void pwm_run(void)
{
  uint32_t now;
  uint32_t next_led_tick = HAL_GetTick() + LED_HALF_PERIOD_MS;
  const uint32_t start = DWT->CYCCNT;

  for (uint32_t i = 0U; i < PWM_LINE_COUNT; i++)
  {
    s_lines[i].next_toggle = start + s_lines[i].half_period_cycles;
  }

  for (;;)
  {
    now = DWT->CYCCNT;

    for (uint32_t i = 0U; i < PWM_LINE_COUNT; i++)
    {
      if ((int32_t)(now - s_lines[i].next_toggle) >= 0)
      {
        if (s_lines[i].level_low != 0U)
        {
          /* Release the open-drain output: the pull-up creates logic high. */
          s_lines[i].port->BSRR = s_lines[i].pin;
          s_lines[i].level_low = 0U;
        }
        else
        {
          s_lines[i].port->BSRR = (uint32_t)s_lines[i].pin << 16U;
          s_lines[i].level_low = 1U;
        }
        s_lines[i].next_toggle += s_lines[i].half_period_cycles;
      }
    }

    if ((int32_t)(HAL_GetTick() - next_led_tick) >= 0)
    {
      next_led_tick += LED_HALF_PERIOD_MS;
      HAL_GPIO_TogglePin(LED6_GPIO_PORT, LED6_PIN);
    }
  }
}

int main(void)
{
  BSP_Init();

  BSP_LedInit(LED6_GPIO_PORT, LED6_PIN);
  pwm_gpio_init();
  pwm_prepare();
  dwt_init();

  printf("\r\n\r\n============================================================\r\n");
  printf("STM32U575 camera connector software PWM continuity test\r\n");
  printf("HCLK = %lu Hz, LED PD2 toggles every %lu ms\r\n",
         (unsigned long)HAL_RCC_GetHCLKFreq(),
         (unsigned long)LED_HALF_PERIOD_MS);
  printf("All lines are open-drain + internal pull-up; 50%% duty target.\r\n");
  printf("Disconnect the camera module before probing the far connector.\r\n");
  printf("============================================================\r\n");
  print_table();
  printf("\r\nPWM output is now running continuously.\r\n");

  pwm_run();
  return 0;
}
