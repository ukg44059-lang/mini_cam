/**
  ******************************************************************************
  * @file    usbd_conf.h
  * @brief   ST USB Device Library 的裁剪配置（手写，不是 CubeMX 生成的）
  *
  * 本工程只做一件事：CDC 虚拟串口，把摄像头的图像帧推到 PC。
  * 所以接口数、配置数都取最小值，内存用静态分配，不引入 malloc。
  ******************************************************************************
  */
#ifndef __USBD_CONF_H
#define __USBD_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32u5xx_hal.h"

/* USBD_Init 的第三个参数：本工程只有一个全速设备实例，编号 0。
   这个宏中间件自己不提供，CubeMX 生成的工程里也是在这里定义的。 */
#define DEVICE_FS                      0

/* ---------------- 规模裁剪 ---------------- */
/* CDC 是「通信接口 + 数据接口」两个 interface */
#define USBD_MAX_NUM_INTERFACES        2U
#define USBD_MAX_NUM_CONFIGURATION     1U
#define USBD_MAX_STR_DESC_SIZ          512U
#define USBD_DEBUG_LEVEL               0U

/* 板子由 USB 供电（Type-C sink），报自供电会让主机的功耗核算不准 */
#define USBD_SELF_POWERED              0U

/* LPM(链路省电) 和自定义字符串描述符都用不上 */
#define USBD_LPM_ENABLED               0U
#define USBD_SUPPORT_USER_STRING_DESC  0U

/* ---------------- 内存分配 ----------------
   USB 库要为「类」分配一块句柄。这里用静态数组顶掉 malloc：
   嵌入式上堆碎片和分配失败都很难查，而我们只有一个 CDC 类，
   编译期就能把大小定死，没必要用堆。 */
#define USBD_malloc                    (void *)USBD_static_malloc
#define USBD_free                      USBD_static_free
#define USBD_memset                    memset
#define USBD_memcpy                    memcpy
#define USBD_Delay                     HAL_Delay

void *USBD_static_malloc(uint32_t size);
void  USBD_static_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CONF_H */
