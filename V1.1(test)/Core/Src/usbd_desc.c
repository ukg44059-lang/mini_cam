/**
  ******************************************************************************
  * @file    usbd_desc.c
  * @brief   USB 设备描述符（手写）
  *
  * 枚举成一个 CDC 虚拟串口。插上 Windows 会多出一个 COM 口，
  * PC 端的 tools/stream_server.py 直接连那个口就行，代码一行都不用改。
  *
  * VID/PID 用的是 ST 官方 CDC 例程那一对（0x0483 / 0x5740）：
  *   - Windows 10 及以后自带 usbser.sys 驱动，认这个 VID/PID，插上即用，不用装驱动
  *   - 这是 ST 给评估板用的公共 ID，自己做产品必须换成自己申请的
  ******************************************************************************
  */

#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_conf.h"

#define USBD_VID                      0x0483
#define USBD_PID                      0x5740
#define USBD_LANGID_STRING            0x0409      /* en-US */
#define USBD_MANUFACTURER_STRING      "STMicroelectronics"
#define USBD_PRODUCT_STRING           "OV3660 Camera Stream (CDC)"
#define USBD_CONFIGURATION_STRING     "CDC Config"
#define USBD_INTERFACE_STRING         "CDC Interface"

static uint8_t *USBD_DevDesc(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_LangIDStrDesc(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_ManufacturerStrDesc(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_ProductStrDesc(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_SerialStrDesc(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_ConfigStrDesc(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_InterfaceStrDesc(USBD_SpeedTypeDef speed, uint16_t *length);

USBD_DescriptorsTypeDef CDC_Desc =
{
  USBD_DevDesc,
  USBD_LangIDStrDesc,
  USBD_ManufacturerStrDesc,
  USBD_ProductStrDesc,
  USBD_SerialStrDesc,
  USBD_ConfigStrDesc,
  USBD_InterfaceStrDesc,
};

/* 描述符缓冲要 4 字节对齐：OTG 的 FIFO 是按字搬的 */
__ALIGN_BEGIN static uint8_t s_dev_desc[USB_LEN_DEV_DESC] __ALIGN_END =
{
  0x12,                       /* bLength */
  USB_DESC_TYPE_DEVICE,       /* bDescriptorType */
  0x00, 0x02,                 /* bcdUSB = 2.00 */
  0x02,                       /* bDeviceClass = CDC（放在设备级，Windows 才好认） */
  0x02,                       /* bDeviceSubClass */
  0x00,                       /* bDeviceProtocol */
  USB_MAX_EP0_SIZE,           /* bMaxPacketSize0 */
  LOBYTE(USBD_VID), HIBYTE(USBD_VID),
  LOBYTE(USBD_PID), HIBYTE(USBD_PID),
  0x00, 0x02,                 /* bcdDevice = 2.00 */
  USBD_IDX_MFC_STR,           /* iManufacturer */
  USBD_IDX_PRODUCT_STR,       /* iProduct */
  USBD_IDX_SERIAL_STR,        /* iSerialNumber */
  USBD_MAX_NUM_CONFIGURATION  /* bNumConfigurations */
};

__ALIGN_BEGIN static uint8_t s_langid_desc[USB_LEN_LANGID_STR_DESC] __ALIGN_END =
{
  USB_LEN_LANGID_STR_DESC,
  USB_DESC_TYPE_STRING,
  LOBYTE(USBD_LANGID_STRING), HIBYTE(USBD_LANGID_STRING),
};

__ALIGN_BEGIN static uint8_t s_str_desc[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;
__ALIGN_BEGIN static uint8_t s_serial_desc[26] __ALIGN_END =
{
  26, USB_DESC_TYPE_STRING,
};

/**
  * @brief 用芯片的 96 位唯一 ID 拼出序列号字符串
  * @note  同一台电脑上插多块板子时，序列号不同才会各自分到独立的 COM 口；
  *        写死一个固定序列号的话，换块板子 Windows 可能沿用旧的枚举记录。
  */
static void serial_from_uid(void)
{
  static const char hex[] = "0123456789ABCDEF";
  uint32_t id[3];
  uint32_t i;
  uint8_t  k = 2;

  id[0] = HAL_GetUIDw0();
  id[1] = HAL_GetUIDw1();
  id[2] = HAL_GetUIDw2();

  /* 取 3 个字里的低 4 字节共 12 个 hex 字符，UTF-16LE 存 */
  for (i = 0; i < 12U; i++)
  {
    uint32_t w = id[i / 4U];
    uint8_t  b = (uint8_t)(w >> ((i % 4U) * 8U));
    s_serial_desc[k++] = (uint8_t)hex[(b >> 4) & 0x0FU];
    s_serial_desc[k++] = 0;
  }
}

static uint8_t *USBD_DevDesc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  *length = (uint16_t)sizeof(s_dev_desc);
  return s_dev_desc;
}

static uint8_t *USBD_LangIDStrDesc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  *length = (uint16_t)sizeof(s_langid_desc);
  return s_langid_desc;
}

static uint8_t *USBD_ProductStrDesc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  USBD_GetString((uint8_t *)USBD_PRODUCT_STRING, s_str_desc, length);
  return s_str_desc;
}

static uint8_t *USBD_ManufacturerStrDesc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  USBD_GetString((uint8_t *)USBD_MANUFACTURER_STRING, s_str_desc, length);
  return s_str_desc;
}

static uint8_t *USBD_SerialStrDesc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  *length = (uint16_t)sizeof(s_serial_desc);
  serial_from_uid();
  return s_serial_desc;
}

static uint8_t *USBD_ConfigStrDesc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  USBD_GetString((uint8_t *)USBD_CONFIGURATION_STRING, s_str_desc, length);
  return s_str_desc;
}

static uint8_t *USBD_InterfaceStrDesc(USBD_SpeedTypeDef speed, uint16_t *length)
{
  (void)speed;
  USBD_GetString((uint8_t *)USBD_INTERFACE_STRING, s_str_desc, length);
  return s_str_desc;
}
