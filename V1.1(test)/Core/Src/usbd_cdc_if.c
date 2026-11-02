/**
  ******************************************************************************
  * @file    usbd_cdc_if.c
  * @brief   USB CDC 虚拟串口的实现（手写）
  ******************************************************************************
  */

#include "usbd_cdc_if.h"
#include "usbd_desc.h"
#include "usbd_core.h"
#include "bsp.h"

/* 一次交给 USB 驱动的最大块。
   OTG 非 DMA 模式下驱动会自己拆成 64 字节的包，块给大一点能少等几次、
   少几次中断往返，直接体现在吞吐上。4KB 远在 XFRSIZ/PKTCNT 的限制之内。 */
#define CDC_TX_CHUNK        4096U

/* OUT 端点的接收缓冲。必须一直挂着接收，否则主机往下发时会被一直 NAK，
   某些串口工具会因此在 open 的时候卡住。 */
static uint8_t s_rx_buf[CDC_DATA_FS_MAX_PACKET_SIZE];

/* 命令环形缓冲：USB 中断里写，主循环里读。
   PC 端的网页控件（换分辨率、限帧率）就是通过它下发到板子的。
   命令都是几个字节的短行，128 字节绰绰有余。 */
#define CDC_RX_RING         128U
static volatile uint8_t  s_rx_ring[CDC_RX_RING];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

/* 发送缓冲指针由 USBD_CDC_SetTxBuffer 直接指向调用者的数据，这里不做拷贝 */
static uint8_t s_tx_dummy[1];

USBD_HandleTypeDef hUsbDeviceFS;

/* 主机有没有打开端口（DTR）。volatile：在 USB 中断里被改，主循环里读 */
static volatile bool s_port_open;

/* ------------------------------------------------------------- 类回调 ----- */

static int8_t CDC_Init_FS(void)
{
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, s_tx_dummy, 0);
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, s_rx_buf);
  return USBD_OK;
}

static int8_t CDC_DeInit_FS(void)
{
  s_port_open = false;
  return USBD_OK;
}

static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
  (void)length;

  switch (cmd)
  {
    case CDC_SET_LINE_CODING:
      /* 主机设的波特率/校验位对 USB CDC 没有实际意义（不存在真实 UART），
         照单全收即可。PC 端 stream_server.py 里那个 --baud 也就无所谓了。 */
      break;

    case CDC_GET_LINE_CODING:
      /* 回一个合法的默认值，不然有些上位机会报错：
         115200 8N1 */
      pbuf[0] = (uint8_t)(115200U & 0xFFU);
      pbuf[1] = (uint8_t)((115200U >> 8) & 0xFFU);
      pbuf[2] = (uint8_t)((115200U >> 16) & 0xFFU);
      pbuf[3] = (uint8_t)((115200U >> 24) & 0xFFU);
      pbuf[4] = 0;      /* 1 个停止位 */
      pbuf[5] = 0;      /* 无校验     */
      pbuf[6] = 8;      /* 8 个数据位 */
      break;

    case CDC_SET_CONTROL_LINE_STATE:
      /*
       * wValue 的 bit0 = DTR。PC 端一 open 串口就会置 1，close 时清 0。
       * 靠它判断「有没有人在收」，没人收就不要硬发，否则每帧都要等到超时。
       *
       * pbuf 指向 setup 包，wValue 在第 2、3 字节。
       */
      {
        USBD_SetupReqTypedef *req = (USBD_SetupReqTypedef *)(void *)pbuf;
        s_port_open = ((req->wValue & 0x0001U) != 0U);
      }
      break;

    default:
      break;
  }
  return USBD_OK;
}

static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len)
{
  uint32_t i;

  /* 收到的字节丢进环形缓冲，主循环用 USBCDC_Read 取走。
     PC 端靠这条通道下发命令（改分辨率、限帧率）。 */
  for (i = 0; i < *Len; i++)
  {
    uint16_t next = (uint16_t)((s_rx_head + 1U) % CDC_RX_RING);
    if (next == s_rx_tail)
    {
      break;                 /* 满了就丢，命令都很短，正常不会满 */
    }
    s_rx_ring[s_rx_head] = Buf[i];
    s_rx_head = next;
  }

  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, s_rx_buf);
  (void)USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  return USBD_OK;
}

uint32_t USBCDC_Read(uint8_t *buf, uint32_t max)
{
  uint32_t n = 0;

  while ((n < max) && (s_rx_tail != s_rx_head))
  {
    buf[n++] = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) % CDC_RX_RING);
  }
  return n;
}

static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
  (void)Buf;
  (void)Len;
  (void)epnum;
  return USBD_OK;
}

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS =
{
  CDC_Init_FS,
  CDC_DeInit_FS,
  CDC_Control_FS,
  CDC_Receive_FS,
  CDC_TransmitCplt_FS
};

/* --------------------------------------------------------------- 对外 ---- */

void USBCDC_Init(void)
{
  if (USBD_Init(&hUsbDeviceFS, &CDC_Desc, DEVICE_FS) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_CDC) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_Interface_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_Start(&hUsbDeviceFS) != USBD_OK)
  {
    Error_Handler();
  }
}

bool USBCDC_IsConfigured(void)
{
  return (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED);
}

bool USBCDC_IsPortOpen(void)
{
  return s_port_open;
}

bool USBCDC_IsReady(void)
{
  return USBCDC_IsConfigured() && s_port_open;
}

uint32_t USBCDC_Write(const uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
  USBD_CDC_HandleTypeDef *hcdc;
  uint32_t sent = 0;
  uint32_t t_end = HAL_GetTick() + timeout_ms;

  if ((data == NULL) || (len == 0U) || !USBCDC_IsConfigured())
  {
    return 0;
  }

  hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassDataCmsit[hUsbDeviceFS.classId];
  if (hcdc == NULL)
  {
    return 0;
  }

  while (sent < len)
  {
    uint32_t chunk = len - sent;
    if (chunk > CDC_TX_CHUNK)
    {
      chunk = CDC_TX_CHUNK;
    }

    /* 等上一块发完 */
    while (hcdc->TxState != 0U)
    {
      if ((int32_t)(HAL_GetTick() - t_end) >= 0)
      {
        return sent;             /* 超时：主机没在取数据 */
      }
    }

    USBD_CDC_SetTxBuffer(&hUsbDeviceFS, (uint8_t *)(uintptr_t)(data + sent), chunk);
    if (USBD_CDC_TransmitPacket(&hUsbDeviceFS) != USBD_OK)
    {
      /* 忙就再等一轮，其他错误直接放弃这一块 */
      if ((int32_t)(HAL_GetTick() - t_end) >= 0)
      {
        return sent;
      }
      continue;
    }
    sent += chunk;
  }

  /* 等最后一块真正发完再返回，否则调用者一改缓冲区就会发出错数据
     （不拷贝的代价，必须等） */
  while (hcdc->TxState != 0U)
  {
    if ((int32_t)(HAL_GetTick() - t_end) >= 0)
    {
      break;
    }
  }

  return sent;
}
