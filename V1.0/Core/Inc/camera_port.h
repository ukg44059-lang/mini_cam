/**
  ******************************************************************************
  * @file    camera_port.h
  * @brief   OV3660 板级适配层：电源时序 / XCLK / SCCB / DCMI+GPDMA
  *
  * ★ 本文件对应的是【自研板 Schematic2】：STM32U575OIY6QTR（WLCSP90）
  *   引脚全部来自网表 Netlist_Schematic2_2026-09-03-02.tel，
  *   AF 编号逐条对照 DS13737 Rev10 的 Table 27(AF0~AF7) / Table 28(AF8~AF15) 核实过。
  *   （不要拿它去对 Nucleo + 底板那套，那是另一份引脚，见 sofeware/ 里的旧版本。）
  *
  *   信号             FPC脚   STM32   AF      备注
  *   ---------------------------------------------------------------------------
  *    3  SIOD / SDA    3     PC1     AF4    I2C3_SDA  4.7k 上拉到 2V8 (R44)
  *    5  SIOC / SCL    5     PC0     AF4    I2C3_SCL  4.7k 上拉到 2V8 (R45)
  *    6  RESETB        6     PA2     GPIO 开漏  4.7k 上拉到 2V8 (R46)，低有效
  *    7  VSYNC         7     PB7     AF10   DCMI_VSYNC
  *    8  PWDN          8     PA1     GPIO 开漏  4.7k 上拉到 2V8 (R47)，高有效
  *    9  HREF          9     PA4     AF10   DCMI_HSYNC
  *   12  Y9 = D9 MSB  12     PE6     AF10   DCMI_D7   ← 注意是 PE6，不是 Nucleo 那版的 PB9
  *   13  XVCLK        13     PA8     AF0    MCO，经 R48 串联
  *   14  Y8 = D8      14     PB8     AF10   DCMI_D6
  *   16  Y7 = D7      16     PB6     AF10   DCMI_D5
  *   17  PCLK         17     PA6     AF4    DCMI_PIXCLK（注意不是 AF10）
  *   18  Y6 = D6      18     PE4     AF10   DCMI_D4
  *   19  Y2 = D2 LSB  19     PC6     AF10   DCMI_D0
  *   20  Y5 = D5      20     PC9     AF4    DCMI_D3（注意不是 AF10）
  *   21  Y3 = D3      21     PC7     AF10   DCMI_D1
  *   22  Y4 = D4      22     PC8     AF10   DCMI_D2
  *    2  AGND         2      GND
  *    4  AVDD 2.8V    4      2V8 经 L1 磁珠
  *   10  DVDD 1.5V    10     1V5
  *   11  DOVDD 2.8V   11     2V8
  *
  *   模组只引出 8 根数据线，接的是 sensor 的 D[9:2]（丢掉 2 个 LSB）。
  *   本板上这 8 根按 D2→DCMI_D0 … D9→DCMI_D7 一一对应，顺序和 Nucleo 那版一致。
  *
  * ⚠ RESETB(PA2) / PWDN(PA1) 一律配成【开漏】输出。
  *   这两个脚外部上拉到 2.8V，而 STM32 的 VDD 是 3.3V，推挽输出高电平会顶到 3.3V，
  *   超过 OV3660 数字脚的绝对最大值 DOVDD+0.3V = 3.1V。开漏 + 外部上拉 = 干净的 2.8V。
  *
  * ★ 摄像头两路电源由 MCU 控制，使能脚都带 4.7k 下拉，上电默认全关：
  *     PE8 -> U8 (TPS7A0215) EN -> 1V5 -> FPC 第 10 脚 (DVDD)
  *     PB1 -> U4 (TPS7A0228) EN -> 2V8 -> FPC 第 11 脚 (DOVDD) + 经 L1 到第 4 脚 (AVDD)
  *
  *   ⚠ 这两个使能脚在板上是通过 3 针跳线选的（网表 H26 / H28）：
  *       跳到 3V3 那侧 = 常开（固件管不了，两路一起上电 → 有闩锁风险）
  *       跳到 MCU 那侧 = PE8 / PB1 控制（★ 推荐，固件才能保证先 1V5 后 2V8）
  *     固件无条件把两个脚拉高，所以两种位置都不会出错，但**只有 MCU 侧才有上电顺序**。
  *
  *   ⚠ LDO 输出到负载之间还有一道串接跳线，不插的话量 LDO 输出正常但负载没电：
  *       H21: USB-C 5V -> 5V_ORI      H22: 3V3_ORI -> 3V3（MCU 自己的电，必须有）
  *       H23: 2V8_ORI -> 2V8          H25: 1V5_ORI -> 1V5
  *
  * ⚠ 和串口/调试口的冲突：本板 USART1 = PA9/PA10，SWD = PA13/PA14，与上面全部引脚不重叠。
  ******************************************************************************
  */
#ifndef __CAMERA_PORT_H
#define __CAMERA_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32u5xx_hal.h"
#include <stdbool.h>

/* XCLK 由 MCO 从 HSI16 直出，16 MHz —— 必须和 ov3660.c 的 OV3660_XCLK_HZ 一致 */
#define CAM_XCLK_HZ             16000000U

/* ---------------- 摄像头控制脚（外部 4.7k 上拉到 2V8） ---------------- */
#define CAM_PWDN_PIN            GPIO_PIN_1      /* PA1, FPC 第8脚，高有效 */
#define CAM_PWDN_PORT           GPIOA
#define CAM_RESET_PIN           GPIO_PIN_2      /* PA2, FPC 第6脚，低有效 */
#define CAM_RESET_PORT          GPIOA

/* ---------------- SCCB = I2C3 ---------------- */
#define CAM_SCL_PIN             GPIO_PIN_0      /* PC0, FPC 第5脚 */
#define CAM_SDA_PIN             GPIO_PIN_1      /* PC1, FPC 第3脚 */
#define CAM_SCCB_PORT           GPIOC

/* ---------------- DCMI 同步信号 ---------------- */
#define CAM_VSYNC_PIN           GPIO_PIN_7      /* PB7  */
#define CAM_VSYNC_PORT          GPIOB
#define CAM_HREF_PIN            GPIO_PIN_4      /* PA4  */
#define CAM_HREF_PORT           GPIOA
#define CAM_PCLK_PIN            GPIO_PIN_6      /* PA6  */
#define CAM_PCLK_PORT           GPIOA

/* ---------------- 摄像头 LDO 使能脚（4.7k 下拉，高有效） ---------------- */
#define CAM_EN_1V5_PIN          GPIO_PIN_8      /* PE8 -> U8 (TPS7A0215) */
#define CAM_EN_1V5_PORT         GPIOE
#define CAM_EN_2V8_PIN          GPIO_PIN_1      /* PB1 -> U4 (TPS7A0228) */
#define CAM_EN_2V8_PORT         GPIOB

/** 单根信号线的电气状态 */
typedef enum
{
  CAM_LINE_PULLED_UP = 0,  /* 有外部上拉把它拉高 -> 2.8V 在，线也通    */
  CAM_LINE_FLOATING,       /* 悬空 -> 没有外部上拉：2.8V 没上，或线没接 */
  CAM_LINE_SHORT_GND       /* 被死死拉到地 -> 短路，或对端器件已损坏    */
} cam_line_state_t;

/** 信号探测结果：把 VSYNC/HREF/PCLK 当普通 GPIO 采样一段时间得到 */
typedef struct
{
  uint32_t sample_count;
  uint32_t vsync_edges;
  uint32_t href_edges;
  uint32_t pclk_edges;
  uint32_t vsync_high;
  uint32_t href_high;
  uint32_t pclk_high;
} cam_probe_t;

/**
  * @brief 按正确顺序打开摄像头的两路 LDO
  *
  *   1V5（DVDD 数字核心） -> 2V8（AVDD/DOVDD）
  *
  * @note  ★ 顺序不能反 ★ 先 2V8 后 1V5 会让 I/O 环带电而数字核心不带电，
  *        OV3660 会闩锁，大电流烧模组。
  *        本板的 3V3（MCU 自己的电）不归固件管，板子一上电就该有。
  */
void CAM_PowerRailsOn(void);

/** @brief 按相反顺序关掉两路 LDO */
void CAM_PowerRailsOff(void);

/**
  * @brief 测一根线的电气状态（先加内部下拉读一次，再加内部上拉读一次）
  *
  *   内部下拉仍读到 1        -> 外部有强上拉，2.8V 到位
  *   内部下拉读 0、内部上拉读 1 -> 悬空
  *   两次都读 0              -> 对地短路
  *
  * @note  测完会把引脚恢复成模拟态，调用者要重新配置
  */
cam_line_state_t CAM_CheckLine(GPIO_TypeDef *port, uint16_t pin);

/** @brief 状态转成中文字符串 */
const char *CAM_LineStateStr(cam_line_state_t s);

/**
  * @brief 测一根线的对地电容（返回内部上拉把它充到高电平所需的 CPU 周期数）
  *
  *   做法：先推挽拉低把电荷放干净 -> 切成输入无上拉 -> 打开内部上拉(约40k)并开始计时
  *         -> 轮询 IDR 直到读出高电平。周期数正比于 RC，也就是正比于线上的电容。
  *
  *   用途：一根「只到排针、后面什么都没接」的脚，和一根「排针 -> PCB走线 ->
  *   FPC座 -> 排线 -> 模组焊盘」的脚，电容差好几倍，周期数会明显不同。
  *
  * @note  测量期间会关中断，测完引脚恢复成模拟态
  */
uint32_t CAM_MeasureLineRC(GPIO_TypeDef *port, uint16_t pin);

/** @brief 打开 XCLK（MCO -> PA8）。AVDD 稳定后才允许起振，所以单独拆出来 */
void CAM_XclkStart(void);

/** @brief 关掉 XCLK */
void CAM_XclkStop(void);

/** @brief 在指定时间内从 PA8 输入回读统计 XCLK 边沿，用于确认 MCO 确实输出 */
uint32_t CAM_XclkEdgeCount(uint32_t duration_us);

/**
  * @brief 初始化 SCCB（I2C3，100kHz，时钟源 HSI16）+ PWDN/RESETB 两个 GPIO
  * @note  只碰 I2C 和这两个控制脚，不碰 DCMI 数据线，
  *        所以调完之后可以先单跑「读芯片 ID」，电流最小最安全
  */
void CAM_SccbInit(void);

/**
  * @brief 按 OV3660 datasheet 2.9 节的时序上电
  *   PWDN 高（掉电）+ RESETB 低（复位） -> XCLK 起振 -> ≥5ms
  *   -> PWDN 拉低 -> ≥1ms -> RESETB 拉高 -> ≥20ms 后才能走 SCCB
  */
void CAM_PowerUp(void);

/** @brief 按相反顺序让传感器掉电 */
void CAM_PowerDown(void);

/** @brief 把 OV3660 驱动的 io 回调绑到本平台的 I2C3 上 */
void CAM_BindSensorIo(void);

/**
  * @brief 扫描整条 I2C 总线，把有 ACK 的 7 位地址填进 found[]
  * @param found 输出数组
  * @param max   数组容量
  * @retval 找到的器件数量
  * @note  OV3660 应该出现在 0x3C。扫不到任何东西 = 传感器没上电/没接上；
  *        扫到别的地址 = 挂的不是这颗片子，或者地址被改过。
  */
uint32_t CAM_SccbScan(uint8_t *found, uint32_t max);

/** @brief 绕过 I2C3 外设，用 GPIO 开漏时序扫描 SCCB ACK（诊断用途） */
uint32_t CAM_SccbScanBitBang(uint8_t *found, uint32_t max);

/** @brief 直接控制 PWDN 脚（true = 释放，靠外部上拉到 2.8V；false = 拉低） */
void CAM_SetPwdn(bool high);

/** @brief 直接控制 RESETB 脚（true = 释放到 2.8V；false = 拉低复位） */
void CAM_SetReset(bool high);

/**
  * @brief 把 VSYNC/HREF/PCLK 配成普通 GPIO 输入，采样 duration_ms 毫秒
  * @note  用来在开 DCMI 之前确认传感器有没有在输出，并推断 VSYNC 极性
  */
void CAM_ProbeSignals(uint32_t duration_ms, cam_probe_t *out);

/**
  * @brief 初始化 DCMI（8bit 硬件同步）+ GPDMA1_Channel0
  * @param vsync_active_high  VSYNC 为高电平时数据【无效】则传 true
  * @param href_active_high   HREF  为高电平时数据【无效】则传 true（OV 一般 false）
  * @param pclk_rising        在 PCLK 上升沿采样传 true
  * @param frame_bytes        一帧多少字节
  *
  * @note  ★ frame_bytes 不是可有可无的参数 ★
  *        HAL_DCMI_Start_DMA 以 0xFFFF 字节为界走两条不同的内部路径，
  *        两条路径要求的 GPDMA 链表结构不同（双节点循环 / 单节点非循环）。
  *        建错的那一侧不会报错，表现是抓帧一直超时。
  *        QVGA 153600 字节走前者，QQVGA 38400 字节走后者。
  */
void CAM_DcmiInit(bool vsync_active_high, bool href_active_high, bool pclk_rising,
                  uint32_t frame_bytes);

/**
  * @brief 同上，多一个 JPEG 模式开关
  * @param jpeg_mode true = 传感器输出的是硬件压缩的 JPEG 码流
  *
  * @note  JPEG 模式下 DCMI 不按行计数，HSYNC 当数据使能用，整帧长度是变的。
  *        frame_bytes 这时候传的是【缓冲上限】而不是确切帧长。
  */
void CAM_DcmiInitEx(bool vsync_active_high, bool href_active_high, bool pclk_rising,
                    uint32_t frame_bytes, bool jpeg_mode);

/**
  * @brief 抓一帧变长的 JPEG
  * @param buf        目标缓冲，4 字节对齐
  * @param max_bytes  缓冲容量（必须是 4 的倍数）
  * @param timeout_ms 超时
  * @retval 这一帧 JPEG 的实际字节数；0 = 没抓到完整帧
  */
uint32_t CAM_CaptureJpeg(uint8_t *buf, uint32_t max_bytes, uint32_t timeout_ms);
/* Nonblocking snapshot start + completion, for bounded JPEG double buffers. */
HAL_StatusTypeDef CAM_CaptureJpegStart(uint8_t *buf, uint32_t max_bytes);
uint32_t CAM_CaptureJpegWait(uint32_t timeout_ms);
uint32_t CAM_GetJpegCaptureMs(void);

/** @brief 上一次 CAM_CaptureJpeg 期间数到的 VSYNC 次数（诊断用） */
uint32_t CAM_GetVsyncCount(void);

/**
  * @brief 抓一帧（SNAPSHOT 模式，抓完自动停）
  * @param buf        目标缓冲区，必须 4 字节对齐
  * @param size_bytes 帧字节数
  * @param timeout_ms 超时
  */
HAL_StatusTypeDef CAM_CaptureFrame(uint8_t *buf, uint32_t size_bytes, uint32_t timeout_ms);

/**
  * @brief 启动一次抓帧但【不等它结束】，立刻返回
  * @note  和 CAM_CaptureWait 配对使用。
  */
HAL_StatusTypeDef CAM_CaptureStart(uint8_t *buf, uint32_t size_bytes);

/** @brief 等 CAM_CaptureStart 启动的那一帧结束 */
HAL_StatusTypeDef CAM_CaptureWait(uint32_t timeout_ms);

/** @brief 反初始化 DCMI，释放引脚 */
void CAM_DcmiDeInit(void);

#ifdef __cplusplus
}
#endif

#endif /* __CAMERA_PORT_H */
