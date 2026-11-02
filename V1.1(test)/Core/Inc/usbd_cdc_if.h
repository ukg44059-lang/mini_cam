/**
  ******************************************************************************
  * @file    usbd_cdc_if.h
  * @brief   USB CDC 虚拟串口对外接口
  *
  * 用途：替掉 UART 当图传通道。UART 在 2 Mbaud 下只有 200 KB/s，
  *       USB 全速批量传输实测能到 700~900 KB/s，帧率能翻好几倍。
  *
  * 典型用法：
  *     USBCDC_Init();
  *     while (!USBCDC_IsReady()) { ... }        // 等主机枚举并打开端口
  *     USBCDC_Write(buf, len, 200);
  ******************************************************************************
  */
#ifndef __USBD_CDC_IF_H
#define __USBD_CDC_IF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_cdc.h"
#include <stdbool.h>

extern USBD_CDC_ItfTypeDef USBD_Interface_fops_FS;

/** @brief 初始化并启动 USB 设备（CDC 类）。失败会走 Error_Handler */
void USBCDC_Init(void);

/** @brief 主机是否已经枚举完成 */
bool USBCDC_IsConfigured(void);

/**
  * @brief 主机是否已经把这个 COM 口打开了（DTR 有效）
  * @note  PC 端 pyserial 一 open 就会置 DTR。没打开就发数据只会被 NAK，
  *        白白占用带宽还会让每次发送都等到超时。
  */
bool USBCDC_IsPortOpen(void);

/** @brief 枚举完成【且】端口已打开 */
bool USBCDC_IsReady(void);

/**
  * @brief 阻塞发送一段数据
  * @param data       要发的数据，函数返回前必须一直有效
  * @param len        字节数
  * @param timeout_ms 总超时
  * @retval 实际发出去的字节数；小于 len 说明超时了（主机没在取数据）
  *
  * @note  内部按块发，每块等上一块传完。不做拷贝，直接从 data 喂 USB FIFO，
  *        所以调用期间不能改动这块内存。
  */
uint32_t USBCDC_Write(const uint8_t *data, uint32_t len, uint32_t timeout_ms);

/**
  * @brief 取出主机发过来的字节（非阻塞）
  * @param buf 输出缓冲
  * @param max 最多取多少
  * @retval 实际取到的字节数，没数据就返回 0
  *
  * @note  PC 端的网页控件靠这条反向通道下发命令（换分辨率、限帧率）。
  *        数据在 USB 中断里进环形缓冲，这里只是取走，不会阻塞图像发送。
  */
uint32_t USBCDC_Read(uint8_t *buf, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CDC_IF_H */
