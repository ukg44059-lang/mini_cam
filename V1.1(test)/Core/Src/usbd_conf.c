/**
  ******************************************************************************
  * @file    usbd_conf.c
  * @brief   USB Device Library 与 STM32 HAL PCD 之间的适配层（手写）
  *
  * 硬件：NUCLEO-U575ZI-Q 的 CN15 USB Type-C，接 STM32U575 的 USB_OTG_FS
  *   PA11 = USB_FS_N (DM)
  *   PA12 = USB_FS_P (DP)
  * 两个脚在底板网表里只走到 Morpho 排针（PA11->U11.14, PA12->U11.12），
  * 没有别的负载，也不和摄像头引脚冲突。
  *
  * 几个关键点（都是 U5 特有、和 F4/F1 不一样的地方）：
  *
  * 1. **必须开 VDDUSB**。U5 的 USB 收发器是独立供电域，复位后是关的，
  *    不调 HAL_PWREx_EnableVddUSB() 的话 USB 完全没反应，还特别难查。
  *
  * 2. **48MHz 时钟走 ICLK**，不是别的芯片上的 CLK48/PLLQ 那套宏。
  *    RCC_PERIPHCLK_ICLK + RCC_CLK48CLKSOURCE_HSI48，并且要打开 HSI48。
  *    HSI48 精度靠 CRS 自动校准（同步到 USB 的 SOF），所以顺手把 CRS 也开了，
  *    不开 CRS 在温漂大的时候可能掉出 USB 的 ±0.25% 容差。
  *
  * 3. **VBUS sensing 关掉**。这块板的 VBUS_SENSE 是接到 PC2 的普通 GPIO（SB6），
  *    并没有接到 OTG 外设的专用 VBUS 脚。开了 vbus_sensing 会一直检测不到
  *    VBUS 而不上拉 DP，主机根本看不到设备。
  *
  * 4. **不要动 UCPD 的 dead battery**。Type-C 靠 CC 线上的 Rd 让主机认出「这是个
  *    设备」并供 VBUS，复位后 dead-battery Rd 是自动生效的。
  *    调 HAL_PWREx_DisableUCPDDeadBattery() 会把 Rd 撤掉，主机随即断 VBUS。
  ******************************************************************************
  */

#include "usbd_core.h"
#include "usbd_conf.h"
#include "bsp.h"          /* Error_Handler */

PCD_HandleTypeDef hpcd_USB_FS;

/* 给 USB 库的类句柄用的静态内存。
   CDC 的 USBD_CDC_HandleTypeDef 约 700 字节，给 1KB 有富余。 */
static uint32_t s_class_mem[1024U / 4U];
static uint8_t  s_class_mem_used;

void *USBD_static_malloc(uint32_t size)
{
  if ((size > sizeof(s_class_mem)) || (s_class_mem_used != 0U))
  {
    return NULL;      /* 只支持一个类，超了宁可返回 NULL 让上层报错 */
  }
  s_class_mem_used = 1U;
  return (void *)s_class_mem;
}

void USBD_static_free(void *p)
{
  (void)p;
  s_class_mem_used = 0U;
}

/* ==========================================================================
   HAL PCD 的 MSP：时钟 / 引脚 / 中断
   ========================================================================== */

void HAL_PCD_MspInit(PCD_HandleTypeDef *pcdHandle)
{
  GPIO_InitTypeDef        gpio = {0};
  RCC_PeriphCLKInitTypeDef pclk = {0};
  RCC_OscInitTypeDef      osc = {0};
  RCC_CRSInitTypeDef      crs = {0};

  if (pcdHandle->Instance != USB_OTG_FS)
  {
    return;
  }

  /* ---- 1. 打开 HSI48，USB 的 48MHz 从这里来 ---- */
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  osc.HSI48State     = RCC_HSI48_ON;
  osc.PLL.PLLState   = RCC_PLL_NONE;     /* 别动已经跑起来的主 PLL */
  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
  {
    Error_Handler();
  }

  /* ---- 2. 把 ICLK（USB/RNG/SDMMC 的中间时钟）切到 HSI48 ---- */
  pclk.PeriphClockSelection = RCC_PERIPHCLK_ICLK;
  pclk.IclkClockSelection   = RCC_CLK48CLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK)
  {
    Error_Handler();
  }

  /* ---- 3. VDDUSB：U5 的 USB 收发器独立供电域，复位后是关的 ---- */
  HAL_PWREx_EnableVddUSB();

  /* ---- 4. PA11/PA12 配成 USB AF ---- */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  gpio.Pin       = GPIO_PIN_11 | GPIO_PIN_12;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF10_USB;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* ---- 5. 外设时钟 + 中断 ---- */
  __HAL_RCC_USB_OTG_FS_CLK_ENABLE();

  HAL_NVIC_SetPriority(OTG_FS_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(OTG_FS_IRQn);

  /* ---- 6. CRS：拿 USB 的 SOF 自动校准 HSI48 ----
     必须放在 USB 时钟配好之后。不开也能枚举，但温漂大时可能掉出 ±0.25% 容差。 */
  __HAL_RCC_CRS_CLK_ENABLE();
  crs.Prescaler             = RCC_CRS_SYNC_DIV1;
  crs.Source                = RCC_CRS_SYNC_SOURCE_USB;
  crs.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
  crs.ReloadValue           = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000U, 1000U);
  crs.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
  crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
  HAL_RCCEx_CRSConfig(&crs);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *pcdHandle)
{
  if (pcdHandle->Instance != USB_OTG_FS)
  {
    return;
  }
  __HAL_RCC_USB_OTG_FS_CLK_DISABLE();
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_11 | GPIO_PIN_12);
  HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
}

/* ==========================================================================
   PCD 回调 -> USB Device 库
   ========================================================================== */

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
  (void)USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd->pData, (uint8_t *)hpcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  (void)USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd->pData, epnum,
                             hpcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  (void)USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd->pData, epnum,
                            hpcd->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
{
  (void)USBD_LL_SOF((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
  /* 这块板只有全速，不用判断枚举速度 */
  (void)USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd->pData, USBD_SPEED_FULL);
  (void)USBD_LL_Reset((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
  (void)USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd->pData);
  /* 注意：这里【不】进 STOP 模式。
     图传要一直跑，进低功耗会把 DCMI/GPDMA 一起停掉。 */
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
{
  (void)USBD_LL_Resume((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  (void)USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  (void)USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
  (void)USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
  (void)USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd->pData);
}

/* ==========================================================================
   USB Device 库 -> PCD
   ========================================================================== */

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
  hpcd_USB_FS.Instance                     = USB_OTG_FS;
  hpcd_USB_FS.Init.dev_endpoints           = 6;
  hpcd_USB_FS.Init.speed                   = PCD_SPEED_FULL;
  hpcd_USB_FS.Init.phy_itface              = PCD_PHY_EMBEDDED;
  hpcd_USB_FS.Init.Sof_enable              = DISABLE;
  hpcd_USB_FS.Init.low_power_enable        = DISABLE;
  hpcd_USB_FS.Init.lpm_enable              = DISABLE;
  hpcd_USB_FS.Init.battery_charging_enable = DISABLE;
  hpcd_USB_FS.Init.dma_enable              = DISABLE;
  /* 见文件头第 3 条：本板 VBUS 没接到 OTG 的专用检测脚，必须关 */
  hpcd_USB_FS.Init.vbus_sensing_enable     = DISABLE;
  hpcd_USB_FS.Init.use_dedicated_ep1       = DISABLE;

  pdev->pData        = &hpcd_USB_FS;
  hpcd_USB_FS.pData  = pdev;

  if (HAL_PCD_Init(&hpcd_USB_FS) != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * FIFO 划分。单位是【32 位字】，不是字节。
   * U575 的 OTG_FS 一共 1.25 KB = 320 字，必须刚好分完不能超：
   *
   *   RX 公共        0x80 = 128 字 = 512 字节
   *   EP0 IN         0x40 =  64 字 = 256 字节
   *   EP1 IN (数据)  0x80 = 128 字 = 512 字节   <- 图传全靠它，给得越大越顺
   *   EP2 IN (命令)  与 EP1 共用剩余，CDC 的中断端点只发 8 字节，够用
   *                                    合计 320 字
   *
   * EP1 IN 给 512 字节意味着一次能压进 8 个 64 字节包，
   * 主机每个 1ms 帧可以连续取走，这是全速下跑满带宽的前提。
   */
  (void)HAL_PCDEx_SetRxFiFo(&hpcd_USB_FS, 0x80);
  (void)HAL_PCDEx_SetTxFiFo(&hpcd_USB_FS, 0, 0x40);
  (void)HAL_PCDEx_SetTxFiFo(&hpcd_USB_FS, 1, 0x80);

  return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
  return (HAL_PCD_DeInit(pdev->pData) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
  return (HAL_PCD_Start(pdev->pData) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
  return (HAL_PCD_Stop(pdev->pData) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                  uint8_t ep_type, uint16_t ep_mps)
{
  return (HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type) == HAL_OK)
         ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return (HAL_PCD_EP_Close(pdev->pData, ep_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return (HAL_PCD_EP_Flush(pdev->pData, ep_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return (HAL_PCD_EP_SetStall(pdev->pData, ep_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return (HAL_PCD_EP_ClrStall(pdev->pData, ep_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;

  if ((ep_addr & 0x80U) == 0x80U)
  {
    return hpcd->IN_ep[ep_addr & 0x7FU].is_stall;
  }
  return hpcd->OUT_ep[ep_addr & 0x7FU].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
  return (HAL_PCD_SetAddress(pdev->pData, dev_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                    uint8_t *pbuf, uint32_t size)
{
  return (HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size) == HAL_OK)
         ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                          uint8_t *pbuf, uint32_t size)
{
  return (HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size) == HAL_OK)
         ? USBD_OK : USBD_FAIL;
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return HAL_PCD_EP_GetRxCount(pdev->pData, ep_addr);
}

void USBD_LL_Delay(uint32_t Delay)
{
  HAL_Delay(Delay);
}
