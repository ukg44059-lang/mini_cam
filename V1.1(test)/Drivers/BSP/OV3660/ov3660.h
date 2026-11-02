/**
  ******************************************************************************
  * @file    ov3660.h
  * @brief   OV3660 图像传感器驱动（SCCB 控制部分，与 MCU 无关）
  *
  *   传感器侧的寄存器操作全部通过 ov3660_io_t 里的两个回调完成，
  *   换 MCU / 换 I2C 外设只要重新填这两个函数指针即可。
  ******************************************************************************
  */
#ifndef __OV3660_H
#define __OV3660_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* SCCB 7 位从机地址（8 位写地址 0x78 / 读地址 0x79） */
#define OV3660_SCCB_ADDR_7BIT   0x3CU

/* 芯片 ID：读 0x300A/0x300B 应该得到 0x3660 */
#define OV3660_CHIP_PID         0x3660U
#define OV3660_REG_CHIP_ID_H    0x300AU
#define OV3660_REG_CHIP_ID_L    0x300BU

typedef enum
{
  OV3660_OK          =  0,
  OV3660_ERR_IO      = -1,   /* SCCB 读写失败（多半是接线 / 上拉 / 供电问题）   */
  OV3660_ERR_ID      = -2,   /* 通信正常但 ID 不对（可能不是 OV3660 或数据线错）*/
  OV3660_ERR_PARAM   = -3,
} ov3660_status_t;

typedef enum
{
  OV3660_PIXFORMAT_RGB565 = 0,
  OV3660_PIXFORMAT_YUV422,
  OV3660_PIXFORMAT_GRAYSCALE,
  OV3660_PIXFORMAT_JPEG,        /* 传感器内部硬件压缩，输出变长 JPEG 码流 */
} ov3660_pixformat_t;

/**
  * 分辨率档位，全部是 4:3
  *
  * OV3660 是 3MP 传感器，满分辨率 2048x1536(QXGA)，内部有缩放器，
  * 下面这些档都是把满幅开窗后缩放输出，所以视场角(FOV)是一样的，
  * 不存在「小分辨率就是裁切一小块」的问题。
  *
  * ⚠ 驱动支持到 QXGA，但**上位机能用到哪一档取决于 MCU 的 RAM**：
  *   STM32U575 只有 768KB SRAM，RGB565 一像素 2 字节，
  *   VGA 640x480 = 614400 字节已经吃掉大半，
  *   再上一档 SVGA 800x600 = 960000 字节【超过芯片总 SRAM】，装不下整帧。
  *   所以 app_stream 里 RGB565 最高只开到 VGA；想更高只能换灰度(1 字节/像素)。
  *   app_stream 会自己按缓冲大小拒绝装不下的档位，不会跑飞。
  */
typedef enum
{
  OV3660_FRAMESIZE_QQVGA = 0,   /* 160 x 120   RGB565 = 38400   */
  OV3660_FRAMESIZE_QVGA,        /* 320 x 240   RGB565 = 153600  */
  OV3660_FRAMESIZE_400X300,     /* 400 x 300   RGB565 = 240000  */
  OV3660_FRAMESIZE_480X360,     /* 480 x 360   RGB565 = 345600  */
  OV3660_FRAMESIZE_VGA,         /* 640 x 480   RGB565 = 614400  <- RGB565 上限 */
  OV3660_FRAMESIZE_SVGA,        /* 800 x 600   灰度   = 480000  */
  OV3660_FRAMESIZE_XGA,         /* 1024 x 768  灰度   = 786432  超 RAM */
  OV3660_FRAMESIZE_UXGA,        /* 1600 x 1200 传感器支持，MCU 存不下 */
  OV3660_FRAMESIZE_QXGA,        /* 2048 x 1536 传感器满分辨率 */
  OV3660_FRAMESIZE_MAX
} ov3660_framesize_t;

/**
  * @brief 平台相关的 SCCB 读写回调
  * @note  OV3660 的寄存器地址是 16 位、数据是 8 位
  */
typedef struct
{
  /** 写一个寄存器，成功返回 0 */
  int (*write_reg)(uint16_t reg, uint8_t val);
  /** 读一个寄存器，成功返回 0，值写进 *val */
  int (*read_reg)(uint16_t reg, uint8_t *val);
  /** 延时 ms */
  void (*delay_ms)(uint32_t ms);
} ov3660_io_t;

/** @brief 绑定平台回调，必须最先调用 */
void ov3660_bind_io(const ov3660_io_t *io);

/**
  * @brief 读芯片 ID
  * @param pid 输出，正常应为 0x3660
  * @retval OV3660_OK / OV3660_ERR_IO
  * @note  这是最轻量的连通性测试：只跑 SCCB，不开数据流，电流最小。
  *        能读到 0x3660 就说明 2.8V/1.5V 供电、XCLK、I2C 接线、RESET/PWDN 时序全对。
  */
int ov3660_read_id(uint16_t *pid);

/**
  * @brief 软复位 + 载入默认寄存器表
  * @note  内部会先写 0x3008=0x82 软复位，等 100ms，再刷 197 条默认配置
  */
int ov3660_reset(void);

/** @brief 设置输出格式 */
int ov3660_set_pixformat(ov3660_pixformat_t fmt);

/**
  * @brief 设置 JPEG 压缩强度（只在 OV3660_PIXFORMAT_JPEG 下有意义）
  * @param qs 量化步长，0x08 ~ 0x3F
  *
  * @note  ★ 方向和直觉相反：数值越【小】画质越好、码流越大 ★
  *        这是量化步长不是「质量分」，很容易搞反。
  *        esp32-camera 默认 0x0C。0x08 很清晰但一帧几十 KB，0x3F 很糊但只有几 KB。
  */
int ov3660_set_quality(uint8_t qs);

/** @brief 设置分辨率（含开窗 / binning / PLL 重算） */
int ov3660_set_framesize(ov3660_framesize_t size);

/** @brief 打开/关闭彩条测试图（不接镜头也能验证 DCMI 通路） */
int ov3660_set_colorbar(bool enable);

/** @brief 水平镜像 / 垂直翻转 */
int ov3660_set_mirror_flip(bool hmirror, bool vflip);

/** @brief 查询某个分辨率档位的像素宽高 */
void ov3660_get_resolution(ov3660_framesize_t size, uint16_t *w, uint16_t *h);

/** @brief 返回上一次 set_framesize 之后算出的 PCLK 频率（Hz），用于估算带宽 */
uint32_t ov3660_get_pclk_hz(void);

/** @brief 直接写一个寄存器（调试用） */
int ov3660_write_reg(uint16_t reg, uint8_t val);

/** @brief 直接读一个寄存器（调试用） */
int ov3660_read_reg(uint16_t reg, uint8_t *val);

#ifdef __cplusplus
}
#endif

#endif /* __OV3660_H */
