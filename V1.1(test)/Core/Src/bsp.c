/**
  ******************************************************************************
  * @file    bsp.c
  * @brief   板级公共初始化实现
  *
  *   系统时钟：MSI 4MHz -> PLL1(N=80,R=2) -> SYSCLK 160MHz（不依赖外部晶振）
  *   供电    ：SMPS（-Q 版本板子自带），VOS Range1，Flash 4 等待周期
  ******************************************************************************
  */

#include "bsp.h"

UART_HandleTypeDef huart1;

static void SystemClock_Config(void);
static void SystemPower_Config(void);
static void MX_ICACHE_Init(void);
static void MX_USART1_UART_Init(void);

void BSP_Init(void)
{
  HAL_Init();
  SystemClock_Config();
  SystemPower_Config();
  MX_ICACHE_Init();
  MX_USART1_UART_Init();

  /* 关掉 printf 的行缓冲，不然日志会一段一段吐出来 */
  setvbuf(stdout, NULL, _IONBF, 0);
}

void BSP_LedInit(GPIO_TypeDef *port, uint16_t pin)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (port == GPIOA)      { __HAL_RCC_GPIOA_CLK_ENABLE(); }
  else if (port == GPIOB) { __HAL_RCC_GPIOB_CLK_ENABLE(); }
  else if (port == GPIOC) { __HAL_RCC_GPIOC_CLK_ENABLE(); }
  else if (port == GPIOD) { __HAL_RCC_GPIOD_CLK_ENABLE(); }
  else if (port == GPIOE) { __HAL_RCC_GPIOE_CLK_ENABLE(); }
  else if (port == GPIOG) { __HAL_RCC_GPIOG_CLK_ENABLE(); }

  /* 先把 ODR 写成"灭"再切输出。本板 LED 是灌电流（拉低才亮），
     写成 RESET 的话会先亮一下 —— 所以这里写的是 LED_OFF_LEVEL */
  HAL_GPIO_WritePin(port, pin, LED_OFF_LEVEL);

  GPIO_InitStruct.Pin   = pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(port, &GPIO_InitStruct);
}

void BSP_UartWrite(const uint8_t *data, uint32_t len)
{
  /* HAL_UART_Transmit 的长度参数是 16 位，一帧图像远超 65535 字节，必须分段发 */
  while (len > 0U)
  {
    uint16_t chunk = (len > 32768U) ? 32768U : (uint16_t)len;

    (void)HAL_UART_Transmit(&huart1, (uint8_t *)data, chunk, HAL_MAX_DELAY);
    data += chunk;
    len  -= chunk;
  }
}

/**
  * @brief 系统时钟配置：MSI(4MHz) -> PLL1(N=80,R=2) -> SYSCLK 160MHz
  */
static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /* 跑 160MHz 必须先切到 Range 1（HAL 内部会同时打开 EPOD 升压） */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /* HSI16 单独打开：摄像头的 XCLK(MCO) 和 I2C3 都用它做时钟源，
     这样 XCLK 频率不受 SYSCLK 配置影响，寄存器表的 PLL 参数才对得上 */
  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_MSI | RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.MSIState            = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.MSIClockRange       = RCC_MSIRANGE_4;      /* MSIS = 4 MHz    */
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_MSI;
  RCC_OscInitStruct.PLL.PLLMBOOST       = RCC_PLLMBOOST_DIV1;
  RCC_OscInitStruct.PLL.PLLM            = 1;                   /* VCO 输入 4 MHz  */
  RCC_OscInitStruct.PLL.PLLN            = 80;                  /* VCO 输出 320MHz */
  RCC_OscInitStruct.PLL.PLLP            = 2;
  RCC_OscInitStruct.PLL.PLLQ            = 2;
  RCC_OscInitStruct.PLL.PLLR            = 2;                   /* SYSCLK  160MHz  */
  RCC_OscInitStruct.PLL.PLLRGE          = RCC_PLLVCIRANGE_0;   /* 4~8MHz 输入档   */
  RCC_OscInitStruct.PLL.PLLFRACN        = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK   | RCC_CLOCKTYPE_SYSCLK |
                                     RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2  |
                                     RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

static void SystemPower_Config(void)
{
  /*
   * ★ 这里【故意不调】 HAL_PWREx_DisableUCPDDeadBattery() ★
   *
   * 这一句是 CubeMX 模板常见的写法（"关掉 UCPD 死电池上拉"），但在本板上是错的。
   *
   * U5 复位后 PWR_UCPDR.UCPD_DBDIS = 0，也就是 dead-battery 下拉默认是【开】的，
   * 它作用在 UCPDx_CC1 / UCPDx_CC2 上 —— 本板这两个脚就是 PA15 / PB15
   * （见 Nucleo 原理图 MB1549 Sheet 6）。底板 CN15 的 CC1/CC2 经 U15(TCPP01-M12)
   * 正好接到这两个脚，而板上【没有】5.1k 的离散 Rd：
   *   TCPP01 只在 dead-battery 期间给 CC 一个 1.1V clamp，MCU 上电后它就把 clamp
   *   撤掉（TCPP01-M12 手册 Fig.19），之后 CC 上唯一的 Rd 就是 MCU 内部这一对。
   *
   * 一旦置 DBDIS=1 把它关掉，Rd 就没了：主机在 CC 上看到开路 -> 撤 VBUS + 不枚举，
   * 症状是「插上一点反应都没有，和没插线一样」。
   * 而本工程用的是 OTG_FS，并没有初始化 UCPD，所以没有任何东西能接管这个 Rd。
   *
   * 详见 README「USB 这块有几个 U5 特有的坑」和 usbd_conf.c 文件头第 4 条。
   */

  /* NUCLEO-U575ZI-Q 是 “-Q” 版，用内部 SMPS 供 VCORE，功耗更低 */
  if (HAL_PWREx_ConfigSupply(PWR_SMPS_SUPPLY) != HAL_OK)
  {
    Error_Handler();
  }

  /* !! 关键 !! PG[2:15] 挂在 VDDIO2 域，不使能这一句 PG2 上的红灯永远不亮 */
  HAL_PWREx_EnableVddIO2();
}

static void MX_ICACHE_Init(void)
{
  if (HAL_ICACHE_ConfigAssociativityMode(ICACHE_1WAY) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_ICACHE_Enable() != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_USART1_UART_Init(void)
{
  huart1.Instance                    = VCP_UART;
  huart1.Init.BaudRate               = VCP_BAUDRATE;
  huart1.Init.WordLength             = UART_WORDLENGTH_8B;
  huart1.Init.StopBits               = UART_STOPBITS_1;
  huart1.Init.Parity                 = UART_PARITY_NONE;
  huart1.Init.Mode                   = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling           = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

/* ---- printf 重定向到 USART1 ---- */
int __io_putchar(int ch)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
  return ch;
}

/**
  * @brief 出错时红灯快闪
  */
void Error_Handler(void)
{
  __disable_irq();
  LD3_GPIO_CLK_ENABLE();
  while (1)
  {
    HAL_GPIO_TogglePin(LD3_GPIO_PORT, LD3_PIN);
    for (volatile uint32_t i = 0; i < 500000U; i++)
    {
      __NOP();
    }
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  printf("assert failed: %s:%lu\r\n", (char *)file, (unsigned long)line);
  Error_Handler();
}
#endif
