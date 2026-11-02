/**
  ******************************************************************************
  * @file    camera_port.c
  * @brief   OV3660 板级适配层实现（自研板 Schematic2 / STM32U575OIY6QTR WLCSP90）
  *
  *   引脚定义和跳线说明见 camera_port.h 顶部的表。
  ******************************************************************************
  */

#include "camera_port.h"
#include "bsp.h"
#include "ov3660.h"
#include <string.h>

/* SCCB 器件地址：HAL 要 8 位形式 */
#define CAM_SCCB_ADDR8          ((uint16_t)(OV3660_SCCB_ADDR_7BIT << 1))

/* R44~R47 未焊：SCCB 与控制脚使用内部弱上拉。
 * HSI16 / PRESC=3 / SCLH=SCLL=199，SCCB 约 10 kHz。
 * 内部上拉接 MCU VDD，需实测传感器输入电压及上升时间。
 */
#define CAM_I2C_TIMING          0x30F0C7C7U

/* ------------------------------------------------------------- 句柄 -------- */
static I2C_HandleTypeDef  hi2c3;
static DCMI_HandleTypeDef hdcmi;
static DMA_HandleTypeDef  hdma_dcmi;

/*
 * ★ 为什么要用链表 DMA ★
 *
 * U5 的 GPDMA 单个 block 最多搬 65535 字节，而 QVGA RGB565 一帧就有 153600 字节。
 * 看 stm32u5xx_hal_dcmi.c 的 HAL_DCMI_Start_DMA()：当字节数超过 0xFFFF 时，
 * 它会把整帧对半拆成若干块做乒乓搬运，而这条路径【只支持链表模式】——
 * 非链表分支直接返回 HAL_ERROR。
 *
 * 而且 HAL 会同时改写「头节点」和「头节点的下一个节点」，所以队列里必须有
 * 两个不同的节点，再设成循环（A->B->A），一个节点填前半块、一个填后半块。
 * 只放一个节点的话 A 的下一个就是自己，两次改写会互相覆盖，第一块的目标地址就错了。
 */
static DMA_NodeTypeDef  s_dma_node_a;
static DMA_NodeTypeDef  s_dma_node_b;
static DMA_QListTypeDef s_dma_queue;

static volatile uint8_t s_frame_done;
static volatile uint8_t s_frame_error;
/* JPEG 变长抓帧靠数 VSYNC 判断帧结束，见 HAL_DCMI_VsyncEventCallback */
static volatile uint32_t s_vsync_count;
static uint8_t *s_jpeg_buf;
static uint32_t s_jpeg_capacity, s_jpeg_start_tick;
static volatile uint32_t s_frame_done_tick;
static uint32_t s_jpeg_capture_ms;

/* ------------------------------------------------------- 摄像头电源 ----- */

/*
 * 本板两路 LDO 的使能脚，都带 4.7k 下拉，上电默认全关：
 *     PE8 -> U8 (TPS7A0215) EN -> 1V5 -> FPC 第 10 脚 (DVDD)
 *     PB1 -> U4 (TPS7A0228) EN -> 2V8 -> FPC 第 11 脚 (DOVDD) + 经 L1 到第 4 脚 (AVDD)
 * 3V3（MCU 自己的电）由板上 USB-C -> H21 -> U12(RT9013) -> H22 提供，不归固件管。
 */
void CAM_PowerRailsOn(void)
{
  GPIO_InitTypeDef gi = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

  /* 先把两个使能脚都拉低（和外部 4.7k 下拉一致），确保从「全关」开始 */
  HAL_GPIO_WritePin(CAM_EN_1V5_PORT, CAM_EN_1V5_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(CAM_EN_2V8_PORT, CAM_EN_2V8_PIN, GPIO_PIN_RESET);

  gi.Mode  = GPIO_MODE_OUTPUT_PP;   /* 使能脚是 3.3V 域的，推挽没问题 */
  gi.Pull  = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_LOW;

  gi.Pin = CAM_EN_1V5_PIN;
  HAL_GPIO_Init(CAM_EN_1V5_PORT, &gi);
  gi.Pin = CAM_EN_2V8_PIN;
  HAL_GPIO_Init(CAM_EN_2V8_PORT, &gi);
  HAL_Delay(5);

  /* 1) 1V5：sensor 数字核心 DVDD。★ 必须最先 ★ */
  HAL_GPIO_WritePin(CAM_EN_1V5_PORT, CAM_EN_1V5_PIN, GPIO_PIN_SET);
  HAL_Delay(10);

  /* 2) 2V8：AVDD + DOVDD。★ 一定要在 1V5 之后 ★
        反过来的话 I/O 环带电、数字核心不带电，OV3660 会闩锁并烧毁 */
  HAL_GPIO_WritePin(CAM_EN_2V8_PORT, CAM_EN_2V8_PIN, GPIO_PIN_SET);
  HAL_Delay(20);
}

void CAM_PowerRailsOff(void)
{
  HAL_GPIO_WritePin(CAM_EN_2V8_PORT, CAM_EN_2V8_PIN, GPIO_PIN_RESET);
  HAL_Delay(5);
  HAL_GPIO_WritePin(CAM_EN_1V5_PORT, CAM_EN_1V5_PIN, GPIO_PIN_RESET);
}

/* ----------------------------------------------------------- 电气自检 ----- */

static void gpio_clk_enable(GPIO_TypeDef *port)
{
  if (port == GPIOA)      { __HAL_RCC_GPIOA_CLK_ENABLE(); }
  else if (port == GPIOB) { __HAL_RCC_GPIOB_CLK_ENABLE(); }
  else if (port == GPIOC) { __HAL_RCC_GPIOC_CLK_ENABLE(); }
  else if (port == GPIOD) { __HAL_RCC_GPIOD_CLK_ENABLE(); }
  else if (port == GPIOE) { __HAL_RCC_GPIOE_CLK_ENABLE(); }
  else if (port == GPIOG) { __HAL_RCC_GPIOG_CLK_ENABLE(); }
}

cam_line_state_t CAM_CheckLine(GPIO_TypeDef *port, uint16_t pin)
{
  GPIO_InitTypeDef gi = {0};
  GPIO_PinState with_pd;
  GPIO_PinState with_pu;

  gpio_clk_enable(port);

  gi.Pin   = pin;
  gi.Mode  = GPIO_MODE_INPUT;
  gi.Speed = GPIO_SPEED_FREQ_LOW;

  /* 第一次：内部下拉。外面若有 4.7k 到 2.8V 的强上拉，这里仍能读到高 */
  gi.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(port, &gi);
  HAL_Delay(2);
  with_pd = HAL_GPIO_ReadPin(port, pin);

  /* 第二次：内部上拉（约 40k 到 3.3V）。悬空的线会被拉高，短地的线仍是低 */
  gi.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(port, &gi);
  HAL_Delay(2);
  with_pu = HAL_GPIO_ReadPin(port, pin);

  HAL_GPIO_DeInit(port, pin);

  if (with_pd == GPIO_PIN_SET)
  {
    return CAM_LINE_PULLED_UP;
  }
  if (with_pu == GPIO_PIN_SET)
  {
    return CAM_LINE_FLOATING;
  }
  return CAM_LINE_SHORT_GND;
}

uint32_t CAM_MeasureLineRC(GPIO_TypeDef *port, uint16_t pin)
{
  const uint32_t TRIALS = 16U;
  const uint32_t LIMIT  = 200000U;      /* 160MHz 下约 1.25ms，够充满了 */
  uint32_t pos;
  uint32_t total = 0;
  uint32_t i;

  gpio_clk_enable(port);

  /* 引脚序号：pin 是 one-hot 掩码，数末尾有多少个 0 就是第几号 */
  pos = 0U;
  while (((pin >> pos) & 1U) == 0U)
  {
    pos++;
  }
  pos *= 2U;                            /* MODER/PUPDR 每个引脚占 2 bit */

  /* 打开 DWT 周期计数器 */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

  for (i = 0; i < TRIALS; i++)
  {
    uint32_t t0;
    uint32_t cyc;
    volatile uint32_t d;

    /* 1) 推挽输出拉低，把线上的电荷彻底放掉 */
    MODIFY_REG(port->PUPDR, 3UL << pos, 0UL << pos);   /* 无上下拉 */
    MODIFY_REG(port->MODER, 3UL << pos, 1UL << pos);   /* 输出     */
    port->BSRR = (uint32_t)pin << 16;                  /* 拉低     */
    for (d = 0; d < 3000U; d++)
    {
      __NOP();
    }

    /* 2) 切成输入、仍然不加上拉，线保持在 0V */
    MODIFY_REG(port->MODER, 3UL << pos, 0UL << pos);
    for (d = 0; d < 50U; d++)
    {
      __NOP();
    }

    /* 3) 打开内部上拉的同一刻开始计时，轮询到读出高电平 */
    __disable_irq();
    t0 = DWT->CYCCNT;
    MODIFY_REG(port->PUPDR, 3UL << pos, 1UL << pos);   /* 内部上拉 ON */
    while (((port->IDR & pin) == 0U) && ((DWT->CYCCNT - t0) < LIMIT))
    {
      /* 空转等待 */
    }
    cyc = DWT->CYCCNT - t0;
    __enable_irq();

    total += cyc;
  }

  HAL_GPIO_DeInit(port, pin);
  return total / TRIALS;
}

const char *CAM_LineStateStr(cam_line_state_t s)
{
  switch (s)
  {
    case CAM_LINE_PULLED_UP: return "有外部上拉  (2.8V 到位，线通)";
    case CAM_LINE_FLOATING:  return "悬空        (没有外部上拉：2.8V 没上，或线没接)";
    default:                 return "对地短路    (线短了，或对端器件已损坏)";
  }
}

/* --------------------------------------------------------------- XCLK ----- */

void CAM_XclkStart(void)
{
  GPIO_InitTypeDef gi = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* HSI16 直出，不分频 -> 16 MHz。
     HSI16 在 bsp.c 的 SystemClock_Config 里已经使能，不受 SYSCLK 配置影响。 */
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSI, RCC_MCODIV_1);

  gi.Pin       = GPIO_PIN_8;          /* PA8 */
  gi.Mode      = GPIO_MODE_AF_PP;
  gi.Pull      = GPIO_NOPULL;
  gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gi.Alternate = GPIO_AF0_MCO;
  HAL_GPIO_Init(GPIOA, &gi);
}

void CAM_XclkStop(void)
{
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_NOCLOCK, RCC_MCODIV_1);
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_8);
}

uint32_t CAM_XclkEdgeCount(uint32_t duration_us)
{
  uint32_t start;
  uint32_t cycles;
  uint32_t edges = 0;
  uint32_t previous;

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

  previous = GPIOA->IDR & GPIO_PIN_8;
  cycles = (SystemCoreClock / 1000000U) * duration_us;
  start = DWT->CYCCNT;

  while ((DWT->CYCCNT - start) < cycles)
  {
    uint32_t current = GPIOA->IDR & GPIO_PIN_8;
    if (current != previous)
    {
      previous = current;
      edges++;
    }
  }

  return edges;
}

/* --------------------------------------------------------------- SCCB ----- */

static int sccb_write(uint16_t reg, uint8_t val)
{
  return (HAL_I2C_Mem_Write(&hi2c3, CAM_SCCB_ADDR8, reg,
                            I2C_MEMADD_SIZE_16BIT, &val, 1, 100) == HAL_OK) ? 0 : -1;
}

static int sccb_read(uint16_t reg, uint8_t *val)
{
  return (HAL_I2C_Mem_Read(&hi2c3, CAM_SCCB_ADDR8, reg,
                           I2C_MEMADD_SIZE_16BIT, val, 1, 100) == HAL_OK) ? 0 : -1;
}

static void sccb_delay(uint32_t ms)
{
  HAL_Delay(ms);
}

void CAM_BindSensorIo(void)
{
  static const ov3660_io_t io = {
    .write_reg = sccb_write,
    .read_reg  = sccb_read,
    .delay_ms  = sccb_delay,
  };
  ov3660_bind_io(&io);
}

void CAM_SccbInit(void)
{
  GPIO_InitTypeDef         gi            = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* --- PWDN(PA1) / RESETB(PA2)：开漏输出，外部 4.7k 上拉到 2.8V ---
     上电默认 PWDN 高（掉电）、RESETB 低（复位中），正好是安全的初始状态 */
  HAL_GPIO_WritePin(CAM_PWDN_PORT,  CAM_PWDN_PIN,  GPIO_PIN_SET);    /* 开漏放开 -> 上拉到 2.8V */
  HAL_GPIO_WritePin(CAM_RESET_PORT, CAM_RESET_PIN, GPIO_PIN_RESET);  /* 拉低 -> 保持复位 */

  gi.Mode  = GPIO_MODE_OUTPUT_OD;   /* ★ 开漏，绝不能用推挽 */
  /* R46/R47 未焊：开漏 + 内部弱上拉。
   * 上拉电源为 MCU VDD，不能保证符合传感器输入电压规格。
   */
  gi.Pull  = GPIO_PULLUP;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  gi.Pin   = CAM_PWDN_PIN | CAM_RESET_PIN;
  HAL_GPIO_Init(CAM_PWDN_PORT, &gi);        /* PA1/PA2 都在 GPIOA */

  /* --- I2C3 时钟源固定为 HSI16 --- */
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_I2C3;
  PeriphClkInit.I2c3ClockSelection   = RCC_I2C3CLKSOURCE_HSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }

  /* PC0=SCL / PC1=SDA，R44/R45 未焊，使用内部弱上拉。 */
  gi.Pin       = CAM_SCL_PIN | CAM_SDA_PIN;
  gi.Mode      = GPIO_MODE_AF_OD;
  gi.Pull      = GPIO_PULLUP;
  gi.Speed     = GPIO_SPEED_FREQ_LOW;
  gi.Alternate = GPIO_AF4_I2C3;
  HAL_GPIO_Init(CAM_SCCB_PORT, &gi);

  __HAL_RCC_I2C3_CLK_ENABLE();

  hi2c3.Instance              = I2C3;
  hi2c3.Init.Timing           = CAM_I2C_TIMING;
  hi2c3.Init.OwnAddress1      = 0;
  hi2c3.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
  hi2c3.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
  hi2c3.Init.OwnAddress2      = 0;
  hi2c3.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c3.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
  hi2c3.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c3, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }
}

uint32_t CAM_SccbScan(uint8_t *found, uint32_t max)
{
  uint32_t n = 0;

  /* I2C 的保留地址是 0x00~0x07 和 0x78~0x7F，只扫中间这段 */
  for (uint8_t a = 0x08; a <= 0x77; a++)
  {
    if (HAL_I2C_IsDeviceReady(&hi2c3, (uint16_t)(a << 1), 2, 5) == HAL_OK)
    {
      if ((found != NULL) && (n < max))
      {
        found[n] = a;
      }
      n++;
    }
  }
  return n;
}

static void sccb_bb_delay(void)
{
  for (volatile uint32_t i = 0; i < 800U; i++)
  {
    __NOP();
  }
}

static bool sccb_bb_address_ack(uint8_t address)
{
  bool ack;

  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SDA_PIN | CAM_SCL_PIN, GPIO_PIN_SET);
  sccb_bb_delay();
  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SDA_PIN, GPIO_PIN_RESET);
  sccb_bb_delay();
  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SCL_PIN, GPIO_PIN_RESET);

  address <<= 1;
  for (uint32_t bit = 0; bit < 8U; bit++)
  {
    HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SDA_PIN,
                      (address & 0x80U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    sccb_bb_delay();
    HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SCL_PIN, GPIO_PIN_SET);
    sccb_bb_delay();
    HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SCL_PIN, GPIO_PIN_RESET);
    address <<= 1;
  }

  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SDA_PIN, GPIO_PIN_SET);
  sccb_bb_delay();
  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SCL_PIN, GPIO_PIN_SET);
  sccb_bb_delay();
  ack = (HAL_GPIO_ReadPin(CAM_SCCB_PORT, CAM_SDA_PIN) == GPIO_PIN_RESET);
  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SCL_PIN, GPIO_PIN_RESET);

  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SDA_PIN, GPIO_PIN_RESET);
  sccb_bb_delay();
  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SCL_PIN, GPIO_PIN_SET);
  sccb_bb_delay();
  HAL_GPIO_WritePin(CAM_SCCB_PORT, CAM_SDA_PIN, GPIO_PIN_SET);
  sccb_bb_delay();

  return ack;
}

uint32_t CAM_SccbScanBitBang(uint8_t *found, uint32_t max)
{
  GPIO_InitTypeDef gi = {0};
  uint32_t n = 0;

  CLEAR_BIT(I2C3->CR1, I2C_CR1_PE);

  gi.Pin   = CAM_SCL_PIN | CAM_SDA_PIN;
  gi.Mode  = GPIO_MODE_OUTPUT_OD;
  gi.Pull  = GPIO_PULLUP;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_WritePin(CAM_SCCB_PORT, gi.Pin, GPIO_PIN_SET);
  HAL_GPIO_Init(CAM_SCCB_PORT, &gi);

  for (uint8_t address = 0x08; address <= 0x77; address++)
  {
    if (sccb_bb_address_ack(address))
    {
      if ((found != NULL) && (n < max))
      {
        found[n] = address;
      }
      n++;
    }
  }

  gi.Mode      = GPIO_MODE_AF_OD;
  gi.Alternate = GPIO_AF4_I2C3;
  HAL_GPIO_Init(CAM_SCCB_PORT, &gi);
  SET_BIT(I2C3->CR1, I2C_CR1_PE);

  return n;
}

void CAM_SetPwdn(bool high)
{
  HAL_GPIO_WritePin(CAM_PWDN_PORT, CAM_PWDN_PIN, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void CAM_SetReset(bool high)
{
  HAL_GPIO_WritePin(CAM_RESET_PORT, CAM_RESET_PIN, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ----------------------------------------------------------- 上电时序 ----- */

void CAM_PowerUp(void)
{
  /* 1) 先确保处于安全的掉电 + 复位状态 */
  HAL_GPIO_WritePin(CAM_PWDN_PORT,  CAM_PWDN_PIN,  GPIO_PIN_SET);    /* PWDN = 高 -> 掉电 */
  HAL_GPIO_WritePin(CAM_RESET_PORT, CAM_RESET_PIN, GPIO_PIN_RESET);  /* RESETB = 低 -> 复位 */
  HAL_Delay(10);

  /* 2) AVDD/DOVDD 此时已由 CAM_PowerRailsOn() 建立，起 XCLK */
  CAM_XclkStart();
  HAL_Delay(10);      /* datasheet t2 要求 ≥5ms */

  /* 3) 退出掉电 */
  HAL_GPIO_WritePin(CAM_PWDN_PORT, CAM_PWDN_PIN, GPIO_PIN_RESET);    /* PWDN = 低 -> 工作 */
  HAL_Delay(5);       /* datasheet t3 要求 ≥1ms */

  /* 4) 释放复位 */
  HAL_GPIO_WritePin(CAM_RESET_PORT, CAM_RESET_PIN, GPIO_PIN_SET);    /* 开漏放开 -> 上拉到 2.8V */
  HAL_Delay(30);      /* datasheet t4 要求 ≥20ms，之后才能走 SCCB */
}

void CAM_PowerDown(void)
{
  HAL_GPIO_WritePin(CAM_RESET_PORT, CAM_RESET_PIN, GPIO_PIN_RESET);
  HAL_Delay(1);
  HAL_GPIO_WritePin(CAM_PWDN_PORT, CAM_PWDN_PIN, GPIO_PIN_SET);
  HAL_Delay(1);
  CAM_XclkStop();
}

/* ----------------------------------------------------------- 信号探测 ----- */

void CAM_ProbeSignals(uint32_t duration_ms, cam_probe_t *out)
{
  GPIO_InitTypeDef gi = {0};
  uint32_t t_end;
  uint8_t  pv = 0, ph = 0, pp = 0;
  uint8_t  first = 1;

  if (out == NULL)
  {
    return;
  }
  for (uint32_t i = 0; i < sizeof(cam_probe_t) / sizeof(uint32_t); i++)
  {
    ((uint32_t *)out)[i] = 0;
  }

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  gi.Mode = GPIO_MODE_INPUT;
  gi.Pull = GPIO_NOPULL;

  gi.Pin = CAM_VSYNC_PIN;                  /* PB7 */
  HAL_GPIO_Init(CAM_VSYNC_PORT, &gi);
  gi.Pin = CAM_HREF_PIN;                   /* PA4 */
  HAL_GPIO_Init(CAM_HREF_PORT, &gi);
  gi.Pin = CAM_PCLK_PIN;                   /* PA6 */
  HAL_GPIO_Init(CAM_PCLK_PORT, &gi);

  t_end = HAL_GetTick() + duration_ms;
  while ((int32_t)(HAL_GetTick() - t_end) < 0)
  {
    uint8_t v = (uint8_t)((CAM_VSYNC_PORT->IDR & CAM_VSYNC_PIN) ? 1U : 0U);
    uint8_t h = (uint8_t)((CAM_HREF_PORT->IDR  & CAM_HREF_PIN)  ? 1U : 0U);
    uint8_t p = (uint8_t)((CAM_PCLK_PORT->IDR  & CAM_PCLK_PIN)  ? 1U : 0U);

    if (!first)
    {
      if (v != pv) { out->vsync_edges++; }
      if (h != ph) { out->href_edges++; }
      if (p != pp) { out->pclk_edges++; }
    }
    first = 0;
    pv = v; ph = h; pp = p;

    out->vsync_high += v;
    out->href_high  += h;
    out->pclk_high  += p;
    out->sample_count++;
  }

  /* 采完把引脚还回去，DCMI 初始化时会重新配成 AF */
  HAL_GPIO_DeInit(CAM_VSYNC_PORT, CAM_VSYNC_PIN);
  HAL_GPIO_DeInit(CAM_HREF_PORT,  CAM_HREF_PIN);
  HAL_GPIO_DeInit(CAM_PCLK_PORT,  CAM_PCLK_PIN);
}

/* ------------------------------------------------------------- DCMI ------- */

/**
 * @brief DCMI 的 11 根引脚配置（自研板 Schematic2）
 * @note  AF 编号见 camera_port.h 的表：
 *        PA6(PIXCLK) 和 PC9(D3) 是 AF4，其余数据/同步线是 AF10
 */
static void dcmi_gpio_init(void)
{
  GPIO_InitTypeDef gi = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

  gi.Mode  = GPIO_MODE_AF_PP;
  gi.Pull  = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

  /* --- AF10 组 --- */
  gi.Alternate = GPIO_AF10_DCMI;

  gi.Pin = GPIO_PIN_4;                                         /* PA4 HREF/HSYNC */
  HAL_GPIO_Init(GPIOA, &gi);

  gi.Pin = GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8;               /* PB6 D5, PB7 VSYNC, PB8 D6 */
  HAL_GPIO_Init(GPIOB, &gi);

  gi.Pin = GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8;               /* PC6 D0, PC7 D1, PC8 D2 */
  HAL_GPIO_Init(GPIOC, &gi);

  gi.Pin = GPIO_PIN_4 | GPIO_PIN_6;                            /* PE4 D4, PE6 D7 */
  HAL_GPIO_Init(GPIOE, &gi);

  /* --- AF4 组：PA6 = PIXCLK, PC9 = D3 --- */
  gi.Alternate = GPIO_AF4_DCMI;

  gi.Pin = GPIO_PIN_6;                                         /* PA6 PIXCLK */
  HAL_GPIO_Init(GPIOA, &gi);

  gi.Pin = GPIO_PIN_9;                                         /* PC9 D3 */
  HAL_GPIO_Init(GPIOC, &gi);
}

void CAM_DcmiInit(bool vsync_active_high, bool href_active_high, bool pclk_rising,
                  uint32_t frame_bytes)
{
  CAM_DcmiInitEx(vsync_active_high, href_active_high, pclk_rising, frame_bytes, false);
}

void CAM_DcmiInitEx(bool vsync_active_high, bool href_active_high, bool pclk_rising,
                    uint32_t frame_bytes, bool jpeg_mode)
{
  /*
   * HAL_DCMI_Start_DMA 内部按帧字节数分成两条完全不同的路径，
   * 两条路径要求的链表结构不一样，建错了就抓不到帧（症状是一直超时）：
   *
   *   frame_bytes >  0xFFFF : 走双节点乒乓，把帧切成 N 块轮流搬，
   *                           HAL 会去改第二个节点，没有第二节点直接报错
   *   frame_bytes <= 0xFFFF : 走单次传输，HAL 只设 Head 节点的长度和地址，
   *                           这时候要是还挂着一个长度为 0 的第二节点、
   *                           而且是循环模式，搬完第一个节点就转进死胡同
   */
  bool multi_node = (frame_bytes > 0xFFFFU);

  dcmi_gpio_init();

  __HAL_RCC_DCMI_PSSI_CLK_ENABLE();
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  /* --- GPDMA1 Channel0：DCMI -> 内存，按字搬，双节点循环链表 --- */
  {
    DMA_NodeConfTypeDef node = {0};

    node.NodeType                      = DMA_GPDMA_LINEAR_NODE;
    node.Init.Request                  = GPDMA1_REQUEST_DCMI_PSSI;
    node.Init.BlkHWRequest             = DMA_BREQ_SINGLE_BURST;
    node.Init.Direction                = DMA_PERIPH_TO_MEMORY;
    node.Init.SrcInc                   = DMA_SINC_FIXED;         /* DCMI->DR 地址固定 */
    node.Init.DestInc                  = DMA_DINC_INCREMENTED;
    node.Init.SrcDataWidth             = DMA_SRC_DATAWIDTH_WORD;
    node.Init.DestDataWidth            = DMA_DEST_DATAWIDTH_WORD;
    node.Init.SrcBurstLength           = 1;
    node.Init.DestBurstLength          = 1;
    node.Init.TransferAllocatedPort    = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
    node.Init.TransferEventMode        = DMA_TCEM_BLOCK_TRANSFER; /* 每块完成都要回调 */
    node.Init.Mode                     = DMA_NORMAL;
    node.TriggerConfig.TriggerPolarity = DMA_TRIG_POLARITY_MASKED;
    node.DataHandlingConfig.DataExchange  = DMA_EXCHANGE_NONE;
    node.DataHandlingConfig.DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED;
    /* 地址和长度由 HAL_DCMI_Start_DMA 在运行时填 */
    node.SrcAddress = 0;
    node.DstAddress = 0;
    node.DataSize   = 0;

    /* 队列可能是上一次 CAM_DcmiInit 留下的，先清干净再重建 */
    (void)HAL_DMAEx_List_ResetQ(&s_dma_queue);

    if (HAL_DMAEx_List_BuildNode(&node, &s_dma_node_a) != HAL_OK) { Error_Handler(); }
    if (HAL_DMAEx_List_InsertNode_Tail(&s_dma_queue, &s_dma_node_a) != HAL_OK) { Error_Handler(); }

    if (multi_node)
    {
      /* 大帧：再挂一个节点并首尾相连，HAL 会拿这两个节点做乒乓 */
      if (HAL_DMAEx_List_BuildNode(&node, &s_dma_node_b) != HAL_OK) { Error_Handler(); }
      if (HAL_DMAEx_List_InsertNode_Tail(&s_dma_queue, &s_dma_node_b) != HAL_OK) { Error_Handler(); }
      if (HAL_DMAEx_List_SetCircularMode(&s_dma_queue) != HAL_OK) { Error_Handler(); }
    }
    else
    {
      /* 小帧：只要一个节点，而且必须【非循环】，搬完就停，
         停下来之后 HAL 会打开 DCMI 的 FRAME 中断，等 VSYNC 结束报告整帧完成 */
      (void)HAL_DMAEx_List_ClearCircularMode(&s_dma_queue);
    }
  }

  hdma_dcmi.Instance                         = GPDMA1_Channel0;
  hdma_dcmi.InitLinkedList.Priority          = DMA_HIGH_PRIORITY;
  hdma_dcmi.InitLinkedList.LinkStepMode      = DMA_LSM_FULL_EXECUTION;
  hdma_dcmi.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT1;
  hdma_dcmi.InitLinkedList.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
  hdma_dcmi.InitLinkedList.LinkedListMode    = multi_node ? DMA_LINKEDLIST_CIRCULAR
                                                            : DMA_LINKEDLIST_NORMAL;
  if (HAL_DMAEx_List_Init(&hdma_dcmi) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMAEx_List_LinkQ(&hdma_dcmi, &s_dma_queue) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DMA_ConfigChannelAttributes(&hdma_dcmi, DMA_CHANNEL_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }

  __HAL_LINKDMA(&hdcmi, DMA_Handle, hdma_dcmi);

  HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
  HAL_NVIC_SetPriority(DCMI_PSSI_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DCMI_PSSI_IRQn);

  /* --- DCMI 本体 ---
     极性三个参数含义（RM0456）：
       VSPolarity/HSPolarity 表示【数据无效】时同步脚上的电平
       PCKPolarity 表示在哪个边沿采样数据 */
  hdcmi.Instance              = DCMI;
  hdcmi.Init.SynchroMode      = DCMI_SYNCHRO_HARDWARE;
  hdcmi.Init.PCKPolarity      = pclk_rising ? DCMI_PCKPOLARITY_RISING : DCMI_PCKPOLARITY_FALLING;
  hdcmi.Init.VSPolarity       = vsync_active_high ? DCMI_VSPOLARITY_HIGH : DCMI_VSPOLARITY_LOW;
  hdcmi.Init.HSPolarity       = href_active_high ? DCMI_HSPOLARITY_HIGH : DCMI_HSPOLARITY_LOW;
  hdcmi.Init.CaptureRate      = DCMI_CR_ALL_FRAME;
  hdcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;
  /* JPEG 模式下 DCMI 不按行计数，HSYNC 只当数据使能用，整帧长度是变的 */
  hdcmi.Init.JPEGMode         = jpeg_mode ? DCMI_JPEG_ENABLE : DCMI_JPEG_DISABLE;
  hdcmi.Init.ByteSelectMode   = DCMI_BSM_ALL;
  hdcmi.Init.ByteSelectStart  = DCMI_OEBS_ODD;
  hdcmi.Init.LineSelectMode   = DCMI_LSM_ALL;
  hdcmi.Init.LineSelectStart  = DCMI_OELS_ODD;
  if (HAL_DCMI_Init(&hdcmi) != HAL_OK)
  {
    Error_Handler();
  }
}

void CAM_DcmiDeInit(void)
{
  HAL_NVIC_DisableIRQ(DCMI_PSSI_IRQn);
  HAL_NVIC_DisableIRQ(GPDMA1_Channel0_IRQn);
  (void)HAL_DCMI_DeInit(&hdcmi);
  (void)HAL_DMAEx_List_UnLinkQ(&hdma_dcmi);
  (void)HAL_DMAEx_List_DeInit(&hdma_dcmi);
  (void)HAL_DMAEx_List_ResetQ(&s_dma_queue);

  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_4 | GPIO_PIN_6);              /* HREF, PIXCLK */
  HAL_GPIO_DeInit(GPIOB, GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8); /* D5, VSYNC, D6 */
  HAL_GPIO_DeInit(GPIOC, GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9);
  HAL_GPIO_DeInit(GPIOE, GPIO_PIN_4 | GPIO_PIN_6);              /* D4, D7 */
}

HAL_StatusTypeDef CAM_CaptureFrame(uint8_t *buf, uint32_t size_bytes, uint32_t timeout_ms)
{
  uint32_t t_end;

  if ((buf == NULL) || (size_bytes == 0U) || ((size_bytes & 3U) != 0U))
  {
    return HAL_ERROR;
  }

  s_frame_done  = 0;
  s_frame_error = 0;

  /* HAL_DCMI_Start_DMA 的 Length 单位是【字】，不是字节 */
  if (HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)buf, size_bytes / 4U) != HAL_OK)
  {
    return HAL_ERROR;
  }

  t_end = HAL_GetTick() + timeout_ms;
  while ((s_frame_done == 0U) && (s_frame_error == 0U))
  {
    if ((int32_t)(HAL_GetTick() - t_end) >= 0)
    {
      (void)HAL_DCMI_Stop(&hdcmi);
      return HAL_TIMEOUT;
    }
  }

  (void)HAL_DCMI_Stop(&hdcmi);
  return (s_frame_error != 0U) ? HAL_ERROR : HAL_OK;
}

/* ------------------------------------------------------- HAL 回调 / 中断 -- */

uint32_t CAM_GetVsyncCount(void)
{
  return s_vsync_count;
}

uint32_t CAM_CaptureJpeg(uint8_t *buf, uint32_t max_bytes, uint32_t timeout_ms)
{
  if (CAM_CaptureJpegStart(buf, max_bytes) != HAL_OK) return 0;
  return CAM_CaptureJpegWait(timeout_ms);
}

HAL_StatusTypeDef CAM_CaptureJpegStart(uint8_t *buf, uint32_t max_bytes)
{
  if ((buf == NULL) || (max_bytes < 64U) || ((max_bytes & 3U) != 0U))
  {
    return HAL_ERROR;
  }
  /* Clear stale EOI markers so a truncated DMA capture cannot reuse old data. */
  memset(buf, 0, max_bytes);
  s_frame_done  = 0;
  s_frame_error = 0;
  s_vsync_count = 0;
  s_jpeg_buf = buf; s_jpeg_capacity = max_bytes;
  s_jpeg_start_tick = HAL_GetTick();
  __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_VSYNCRI |
                       DCMI_FLAG_OVRRI | DCMI_FLAG_ERRRI);
  if (HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)buf, max_bytes / 4U) != HAL_OK)
  {
    s_jpeg_buf = NULL;
    return HAL_ERROR;
  }
  /* JPEG DMA is capacity-based and will normally never fill. Enable the
   * hardware frame interrupt explicitly, rather than waiting for DMA full. */
  __HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_FRAME | DCMI_IT_VSYNC | DCMI_IT_ERR | DCMI_IT_OVR);
  return HAL_OK;
}

uint32_t CAM_CaptureJpegWait(uint32_t timeout_ms)
{
  uint8_t *buf = s_jpeg_buf;
  uint32_t max_bytes = s_jpeg_capacity;
  uint32_t len = 0;
  if (buf == NULL) return 0;

  /*
   * 等一帧结束。三种情况都要收：
   *   s_vsync_count >= 2  正常：一次是帧开始，一次是帧结束
   *   s_frame_done        码流把整个缓冲填满了（说明 max_bytes 给小了）
   *   s_frame_error       DMA 出错
   */
  while ((s_vsync_count < 2U) && (s_frame_done == 0U) && (s_frame_error == 0U))
  {
    if ((uint32_t)(HAL_GetTick() - s_jpeg_start_tick) >= timeout_ms)
    {
      break;
    }
  }

  /* VSYNC 中断来的时候，DCMI FIFO 和 DMA 里可能还有几十字节没落到内存，
     给一点时间让它排空，不然扫不到结尾的 FFD9 */
  if (!s_frame_done || (uint32_t)(HAL_GetTick() - s_frame_done_tick) < 2U)
    HAL_Delay(2);
  (void)HAL_DCMI_Stop(&hdcmi);
  s_jpeg_capture_ms = (s_frame_done ? s_frame_done_tick : HAL_GetTick()) - s_jpeg_start_tick;
  s_jpeg_buf = NULL;

  if (s_frame_error != 0U)
  {
    return 0;
  }

  /*
   * 算实际长度：从头扫到第一个 FFD9。
   *
   * 这个办法之所以可靠，是因为 JPEG 熵编码段里的 0xFF 一律做了字节填充
   * （后面强制跟一个 0x00），所以 FF D9 这个组合【只可能】是 EOI 标记本身，
   * 不会在图像数据里偶然出现。比起去解析 GPDMA 的剩余计数要稳得多，
   * 后者在多节点链表模式下很难算准。
   */
  if ((buf[0] != 0xFFU) || (buf[1] != 0xD8U))
  {
    return 0;                     /* 开头不是 SOI，这一帧不完整 */
  }
  for (uint32_t i = 2U; i < (max_bytes - 1U); i++)
  {
    if ((buf[i] == 0xFFU) && (buf[i + 1U] == 0xD9U))
    {
      len = i + 2U;
      break;
    }
  }
  return len;                     /* 0 = 没找到 EOI，帧不完整 */
}

uint32_t CAM_GetJpegCaptureMs(void) { return s_jpeg_capture_ms; }

HAL_StatusTypeDef CAM_CaptureStart(uint8_t *buf, uint32_t size_bytes)
{
  if ((buf == NULL) || (size_bytes == 0U) || ((size_bytes & 3U) != 0U))
  {
    return HAL_ERROR;
  }

  s_frame_done  = 0;
  s_frame_error = 0;

  /* Length 单位是【字】不是字节，和 CAM_CaptureFrame 一致 */
  return HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)buf, size_bytes / 4U);
}

HAL_StatusTypeDef CAM_CaptureWait(uint32_t timeout_ms)
{
  uint32_t t_end = HAL_GetTick() + timeout_ms;

  while ((s_frame_done == 0U) && (s_frame_error == 0U))
  {
    if ((int32_t)(HAL_GetTick() - t_end) >= 0)
    {
      (void)HAL_DCMI_Stop(&hdcmi);
      return HAL_TIMEOUT;
    }
  }

  (void)HAL_DCMI_Stop(&hdcmi);
  return (s_frame_error != 0U) ? HAL_ERROR : HAL_OK;
}

/*
 * JPEG 是变长的，DMA 永远填不满 -> DMA 传输完成事件不会来 ->
 * HAL 也就永远不会打开 DCMI 的 FRAME 中断（它是在 DCMI_DMAXferCplt 里才使能的）。
 * 所以 JPEG 模式下只能靠 VSYNC 判断一帧结束。
 * HAL_DCMI_Init 默认就使能了 DCMI_IT_VSYNC，不用额外开。
 */
void HAL_DCMI_VsyncEventCallback(DCMI_HandleTypeDef *phdcmi)
{
  if (phdcmi->Instance == DCMI)
  {
    s_vsync_count++;
  }
}

void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *phdcmi)
{
  if (phdcmi->Instance == DCMI)
  {
    s_frame_done = 1;
    s_frame_done_tick = HAL_GetTick();
  }
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *phdcmi)
{
  if (phdcmi->Instance == DCMI)
  {
    s_frame_error = 1;
  }
}

void DCMI_PSSI_IRQHandler(void)
{
  HAL_DCMI_IRQHandler(&hdcmi);
}

void GPDMA1_Channel0_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_dcmi);
}
