/**
  ******************************************************************************
  * @file    rf_prop.h
  * @brief   CC1310 Sub-1GHz 专有模式（prop mode）裸机射频层
  *
  * 不依赖 TI-RTOS 的 RF 驱动，直接走 RF core 的门铃(doorbell)提交命令。
  *
  * 用法：
  *     RFP_Init();                       // 上电 + 打补丁 + 配电台 + 设频率
  *     RFP_Send(buf, len);               // 发一包
  *
  * ---------------------------------------------------------------------------
  * ★ 频段配置见 rf_prop_band.h ★
  * 那里的寄存器覆盖表是 SmartRF Studio 针对【具体频段 + 具体调制参数】生成的，
  * 换频段必须整块替换，不能只改 centerFreq。详见该文件的说明。
  ******************************************************************************
  */
#ifndef __RF_PROP_H
#define __RF_PROP_H

#include <stdint.h>
#include <stdbool.h>

/** 一包最大负载。CC1310 只有 20KB RAM，别开太大 */
#define RFP_MAX_PAYLOAD     255U

typedef enum
{
    RFP_OK = 0,
    RFP_ERR_POWER,          /* RF core 电源域没起来 */
    RFP_ERR_XOSC,           /* 24MHz 晶振没切过去 */
    RFP_ERR_SETUP,          /* CMD_PROP_RADIO_DIV_SETUP 失败 */
    RFP_ERR_FS,             /* CMD_FS 失败（合成器没锁上） */
    RFP_ERR_TX,             /* CMD_PROP_TX 失败 */
    RFP_ERR_PARAM,
    RFP_ERR_RX,
    RFP_ERR_RX_TIMEOUT,
} rfp_status_t;

/**
  * @brief 射频初始化：电源域 -> 晶振 -> 补丁 -> 电台配置 -> 设频率
  * @retval RFP_OK 表示合成器已经锁在目标频率上，可以发了
  *
  * @note  这一串每一步都可能失败，返回值要看，别 (void) 掉 ——
  *        射频调试最怕的就是"哪一步没成功不知道"。
  */
rfp_status_t RFP_Init(void);

/**
  * @brief 发一包（阻塞到发完）
  * @param data 负载
  * @param len  长度，<= RFP_MAX_PAYLOAD
  */
rfp_status_t RFP_Send(const uint8_t *data, uint8_t len);

/* Receive one CRC-valid packet. RFP_ERR_RX_TIMEOUT means no packet arrived. */
rfp_status_t RFP_Receive(uint8_t *data, uint8_t capacity,
                         uint8_t *received_len, uint16_t timeout_ms);

/** @brief 把状态码转成中文，方便打日志 */
const char *RFP_StatusStr(rfp_status_t s);

/** @brief 返回最近一次命令的 RF core 状态字，出错时用来定位 */
uint16_t RFP_LastCmdStatus(void);

#endif /* __RF_PROP_H */
