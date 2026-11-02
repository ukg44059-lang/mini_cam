/**
  ******************************************************************************
  * @file    app_stream.c
  * @brief   OV3660 实时串口图传：连续抓帧 -> 串口发到 PC -> 网页显示
  *
  * 和 app_camera.c 的区别：
  *   app_camera 是【一次性验证】，逐阶段自检、抓一帧就停下等按键；
  *   app_stream 是【连续串流】，初始化通过后就一直抓一直发，不再打印文字。
  *
  * 帧率是被串口带宽卡住的，不是被摄像头卡住的：
  *
  *   波特率 2 Mbaud，8N1 一个字节要 10 位 -> 200000 字节/秒
  *     QQVGA 160x120 RGB565 = 38400 字节/帧 -> 理论 5.2 fps
  *     QVGA  320x240 RGB565 = 153600 字节/帧 -> 理论 1.3 fps
  *
  * 为了把帧率榨到接近理论值，这里用【双缓冲 + 抓发重叠】：
  *   DCMI 往 buffer B 里搬的同时，CPU 把 buffer A 从串口发出去。
  *   不重叠的话是 (抓帧时间 + 发送时间)，重叠之后是 max(抓帧, 发送)。
  *
  * 串口帧格式（和 app_camera.c 的 dump_frame 保持一致，tools 下两个脚本通用）：
  *   magic            4 字节   "OV36"
  *   width            2 字节   小端
  *   height           2 字节   小端
  *   bytes_per_pixel  1 字节
  *   seq              1 字节   帧序号低 8 位，PC 端靠它发现丢帧
  *   payload          w*h*bpp 字节，RGB565 大端（高字节先出）
  *   checksum         4 字节   payload 所有字节之和，小端
  *
  * PC 端配套：  python tools/stream_server.py COM17
  *              然后浏览器打开 http://localhost:8080
  *
  * 图传期间 LED 保持关闭。
  ******************************************************************************
  */

#include "bsp.h"
#include "camera_port.h"
#include "ov3660.h"
#include <string.h>
#ifdef STREAM_TRANSPORT_DUAL
#include "cc1310_link.h"
#endif

#ifdef STREAM_TRANSPORT_USB
#include "usbd_cdc_if.h"
#endif

/* ---------------------------------------------------------- 传输通道 -----
 * 图像走哪条路由 Makefile 的 TRANSPORT 决定：
 *
 *   usb  (默认)  CN15 的 USB Type-C，CDC 虚拟串口。全速批量传输，实测 ~700 KB/s
 *   uart         ST-LINK 虚拟串口。2 Mbaud 封顶 200 KB/s
 *
 * 两种情况下 printf 的文字日志都走 ST-LINK 串口，
 * 所以调试信息和图像数据是分开的两个口，不会互相污染。
 */
#ifdef STREAM_TRANSPORT_USB
  #define STREAM_LINK_NAME    "USB CDC (CN15 Type-C)"

  /**
    * @brief 发送超时，按【这次要发多少字节】算，不能用固定值
    *
    * ★ 踩过的坑 ★
    * 原来写死 300ms。USB 实测约 800 KB/s，300ms 只够发 240KB，
    * 于是 480x360(345600 字节) 以上的每一帧都发到一半就超时返回，
    * PC 端收到的永远是残帧、校验永远不过 ——
    * 症状是「小分辨率好好的，一切到大分辨率就一帧都收不到」，
    * 而且板子日志显示切换成功，很容易误判成 DCMI 或传感器的问题。
    *
    * 这里按最低 128 KB/s 估算（实际的六分之一，留足余量），
    * 再加 500ms 固定余量。主机真的不收了照样会超时退出，不会卡死。
    */
  static inline uint32_t stream_tx_timeout(uint32_t n)
  {
    return 500U + (n / 128U);
  }

  static inline void   stream_link_init(void)  { USBCDC_Init(); }
  static inline bool   stream_link_ready(void) { return USBCDC_IsReady(); }
  static inline uint32_t stream_link_write(const uint8_t *d, uint32_t n)
  {
    return USBCDC_Write(d, n, stream_tx_timeout(n));
  }
#elif defined(STREAM_TRANSPORT_DUAL)
  #define STREAM_LINK_NAME "UART + SPI2 -> CC1310 UART"
  static inline void stream_link_init(void) { CC_LinkInit(); }
  static inline bool stream_link_ready(void) { return true; }
  static inline uint32_t stream_link_write(const uint8_t *d, uint32_t n)
  {
    return CC_LinkWrite(d, n);
  }
#else
  #define STREAM_LINK_NAME    "ST-LINK 虚拟串口"
  static inline void   stream_link_init(void)  { }
  static inline bool   stream_link_ready(void) { return true; }
  static inline uint32_t stream_link_write(const uint8_t *d, uint32_t n)
  {
    BSP_UartWrite(d, n);
    return n;
  }
#endif

/* ------------------------------------------------------------- 配置 ------- */
/* 开机默认分辨率：Makefile 里 FRAMESIZE=QQVGA / QVGA 覆盖。
   运行时还能通过命令通道随时切换，见 handle_commands()。 */
#ifndef STREAM_FRAMESIZE
#define STREAM_FRAMESIZE    OV3660_FRAMESIZE_QQVGA
#endif

/*
 * ---------------------------------------------------------------- 缓冲池 ---
 *
 * 整个可用 RAM 里划一块固定的池子，运行时按当前分辨率决定怎么用：
 *
 *   帧 <= 池子的一半   -> 当【两个】缓冲用，抓发重叠，帧率高
 *   帧 >  池子的一半   -> 整块当【一个】缓冲用，抓完才能发，帧率减半但能开大图
 *
 * 池子取 614400 字节（正好是 VGA 640x480 RGB565 一整帧）。
 *
 * 为什么就到这儿为止：STM32U575 一共 768KB = 786432 字节 SRAM，
 * 非缓冲部分（栈、HAL、USB、驱动）占约 7.5KB。
 * 再上一档 SVGA 800x600 RGB565 要 960000 字节，【比芯片总 SRAM 还大】，
 * 不管怎么优化都放不下整帧。想再往上只能减到 1 字节/像素的灰度。
 */
#define STREAM_POOL_BYTES   (640U * 480U * 2U)      /* 614400 */
#define STREAM_HALF_POOL    (STREAM_POOL_BYTES / 2U)

/* 抓帧超时。分辨率越大传感器越慢（VGA 只有约 4.4fps，一帧 227ms），
   所以要按最慢的那档给余量 */
#define STREAM_CAP_TIMEOUT  1200U

/* DMA 目标必须 4 字节对齐 */
static uint8_t s_pool[STREAM_POOL_BYTES] __attribute__((aligned(4)));

/* 指向池子里的两个缓冲。单缓冲模式下两个指针相同 */
static uint8_t *s_buf[2];
static bool     s_double_buffered;

static uint32_t s_seq;

/* ---- 运行时状态（可被 PC 端命令改） ---- */
static ov3660_framesize_t s_framesize = STREAM_FRAMESIZE;
static uint16_t s_w;
static uint16_t s_h;
/* 输出格式，也是帧头里的 fmt 字段：0=JPEG(变长) 1=灰度 2=RGB565 */
#define STREAM_FMT_JPEG   0U
#define STREAM_FMT_GRAY   1U
#define STREAM_FMT_RGB565 2U
static uint8_t  s_format = STREAM_FMT_RGB565;
static uint8_t  s_bpp = 2U;          /* RGB565=2，灰度=1，JPEG 无意义 */

/* JPEG 量化步长，越小画质越好码流越大。0x0C 是 esp32-camera 的默认值 */
static uint8_t  s_jpeg_qs = 0x0CU;

/* 用户是否要求 JPEG 模式（和 s_format 分开：切分辨率时要靠它决定配哪种格式） */
static bool     s_want_jpeg;
static uint32_t s_frame_bytes;

/* 帧率上限。0 = 不限（跑到传感器或链路的极限为止）。
   限帧的做法是「照抓不误，但到点才发」：抓帧本来就是 DMA 后台跑的，
   不占 CPU，限的是链路带宽和 PC 的解码开销，同时保证发出去的是最新的一帧。 */
static uint32_t s_min_period_ms;
static uint32_t s_last_tx_ms;
static bool s_jpeg_pipeline = true;
static bool s_uart_mirror = true;
static bool s_perf_log = true;
static unsigned s_spi_level = 2U;
volatile uint32_t g_stream_capture_ms, g_stream_send_ms, g_stream_jpeg_bytes;

/* DCMI 极性，pick_polarity() 试出来之后要在切分辨率时复用 */
static bool s_pol_vs;
static bool s_pol_hs;
static bool s_pol_pclk;

/* LED1~LED4 share PG14 and are active low: keep all four off while streaming. */
static void white_leds_off_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  LED_W_GPIO_CLK_ENABLE();
  HAL_GPIO_WritePin(LED_W_GPIO_PORT, LED_W_PIN, LED_OFF_LEVEL);
  gpio.Pin   = LED_W_PIN;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_W_GPIO_PORT, &gpio);
}

/* 网页控制命令走同一条 UART。图像发送一帧要占用约 200 ms，不能在
   主循环里轮询 RXNE，否则 R/M/F/Q 命令会在发送期间发生 overrun。 */
#ifndef STREAM_TRANSPORT_USB
#define STREAM_UART_RX_RING  64U
static volatile uint8_t  s_uart_rx_ring[STREAM_UART_RX_RING];
static volatile uint16_t s_uart_rx_head;
static volatile uint16_t s_uart_rx_tail;
static uint8_t           s_uart_rx_byte;

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == VCP_UART)
  {
    uint16_t next = (uint16_t)((s_uart_rx_head + 1U) % STREAM_UART_RX_RING);
    if (next != s_uart_rx_tail)
    {
      s_uart_rx_ring[s_uart_rx_head] = s_uart_rx_byte;
      s_uart_rx_head = next;
    }
    (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == VCP_UART)
  {
    volatile uint32_t ignored = huart->Instance->ISR;
    ignored = huart->Instance->RDR;
    (void)ignored;
    huart->RxState = HAL_UART_STATE_READY;
    (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
  }
}

void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}

static void stream_uart_rx_init(void)
{
  HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
}
#endif

/* ------------------------------------------------------------- 工具 ------- */

static void die(const char *what, const char *hint)
{
  printf("\r\n[失败] %s\r\n%s\r\n", what, hint);
  printf("LED6 常亮（本板低电平点亮），程序停在这里。\r\n");

  BSP_LedInit(LD3_GPIO_PORT, LD3_PIN);
  HAL_GPIO_WritePin(LD3_GPIO_PORT, LD3_PIN, LED_ON_LEVEL);
  while (1)
  {
    __NOP();
  }
}

/**
  * @brief 把一帧按上面的格式发出去
  * @retval true 整帧都发出去了；false 中途超时，这一帧作废
  */
static bool send_frame(const uint8_t *payload, uint32_t len)
{
  uint8_t  head[14];
  uint8_t  tail[4];
  uint32_t sum = 0;
  uint32_t i;

  /* magic 用 "OV3S"（S = stream）和 app_camera 的 "OV36" 区分开：
     两边帧头长度不一样，混起来解析会一直对不上。 */
  head[0]  = 'O';
  head[1]  = 'V';
  head[2]  = '3';
  head[3]  = 'S';
  head[4]  = (uint8_t)(s_w & 0xFFU);
  head[5]  = (uint8_t)(s_w >> 8);
  head[6]  = (uint8_t)(s_h & 0xFFU);
  head[7]  = (uint8_t)(s_h >> 8);
  head[8]  = s_format;                      /* 0=JPEG 1=灰度 2=RGB565 */
  head[9]  = (uint8_t)(s_seq & 0xFFU);
  /* 显式长度：JPEG 是变长的，PC 端没法从 w*h*bpp 推出来 */
  head[10] = (uint8_t)(len & 0xFFU);
  head[11] = (uint8_t)((len >> 8) & 0xFFU);
  head[12] = (uint8_t)((len >> 16) & 0xFFU);
  head[13] = (uint8_t)((len >> 24) & 0xFFU);

  for (i = 0; i < len; i++)
  {
    sum += payload[i];
  }
  tail[0] = (uint8_t)(sum & 0xFFU);
  tail[1] = (uint8_t)((sum >> 8) & 0xFFU);
  tail[2] = (uint8_t)((sum >> 16) & 0xFFU);
  tail[3] = (uint8_t)((sum >> 24) & 0xFFU);

#ifdef STREAM_TRANSPORT_DUAL
  /* Always finish the direct frame, even if the SPI receiver is unavailable. */
  if (s_uart_mirror) {
    BSP_UartWrite(head, sizeof(head));
    BSP_UartWrite(payload, len);
    BSP_UartWrite(tail, sizeof(tail));
  }
#endif

  /*
   * 头、数据、尾分三次发。
   * USB 通道下每次 write 都会等到真正发完才返回（不做拷贝，缓冲必须保持有效），
   * 任何一段没发全就说明主机没在收，直接放弃这一帧 —— 宁可丢帧，
   * 也不能发半截数据出去，那样 PC 端要靠重新找帧头才能恢复。
   */
  if (stream_link_write(head, sizeof(head)) != sizeof(head))
  {
    return false;
  }
  if (stream_link_write(payload, len) != len)
  {
    return false;
  }
  if (stream_link_write(tail, sizeof(tail)) != sizeof(tail))
  {
    return false;
  }
  return true;
}

/**
  * @brief 试出能抓到帧的 DCMI 极性组合
  * @param vs,hs,pclk 输出，试出来的那一组。
  *        后面运行时切分辨率要重新初始化 DCMI，得复用同一组极性，
  *        总不能每次切都再试一遍。
  */
static bool pick_polarity(bool *vs, bool *hs, bool *pclk)
{
  static const struct { bool vs, hs, pclk; } combos[] = {
    { false, false, true  },   /* 验证 demo 实测通过的那组 */
    { true,  false, true  },
    { false, false, false },
    { true,  false, false },
  };
  uint32_t i;

  for (i = 0; i < (sizeof(combos) / sizeof(combos[0])); i++)
  {
    CAM_DcmiInit(combos[i].vs, combos[i].hs, combos[i].pclk, s_frame_bytes);

    if (CAM_CaptureFrame(s_buf[0], s_frame_bytes, STREAM_CAP_TIMEOUT) == HAL_OK)
    {
      printf("  DCMI 极性：VSYNC=%s HSYNC=%s PCLK=%s\r\n",
             combos[i].vs ? "HIGH" : "LOW",
             combos[i].hs ? "HIGH" : "LOW",
             combos[i].pclk ? "RISING" : "FALLING");
      *vs   = combos[i].vs;
      *hs   = combos[i].hs;
      *pclk = combos[i].pclk;
      return true;
    }

    CAM_DcmiDeInit();
  }
  return false;
}

/**
  * @brief 切换分辨率（运行时）
  *
  * 顺序不能乱：
  *   1. 先把 DCMI 停掉并反初始化 —— 传感器改了输出尺寸而 DCMI 还按旧尺寸收，
  *      DMA 就永远凑不满，一直超时
  *   2. 改传感器寄存器
  *   3. 按【新的帧字节数】重新初始化 DCMI
  *      ★ 这一步的 frame_bytes 参数是关键：HAL 以 0xFFFF 字节为界走两条
  *        不同的 DMA 链表路径，QQVGA(38400) 和 QVGA(153600) 恰好分属两侧，
  *        用旧尺寸初始化会抓不到帧。详见 camera_port.h 里 CAM_DcmiInit 的说明。
  *
  * @retval true 切换成功
  */
static bool apply_framesize(ov3660_framesize_t fs)
{
  uint16_t w;
  uint16_t h;
  uint8_t  bpp;
  uint32_t bytes;

  if (fs >= OV3660_FRAMESIZE_MAX)
  {
    return false;
  }

  ov3660_get_resolution(fs, &w, &h);

  if (s_want_jpeg)
  {
    /*
     * JPEG 模式：传感器内部压缩，出来的是变长码流，和分辨率不成正比。
     * 一帧顶多几十 KB，池子 614400 字节随便放，
     * 所以【所有分辨率档位都能用】，包括 2048x1536 满分辨率 ——
     * 这正是硬件 JPEG 相对原始像素最大的价值：不再受 RAM 限制。
     */
    bpp   = 0U;
    /* VGA and below: two independent 300 KiB compressed buffers. Larger
     * formats retain the full capacity and single-buffer capture path. */
    bytes = (s_jpeg_pipeline && w <= 640U && h <= 480U)
          ? STREAM_HALF_POOL : STREAM_POOL_BYTES;
  }
  else
  {
    /* 原始像素：大于 VGA 的档位改用灰度。RGB565 一像素 2 字节，
       SVGA 就要 960000 字节，比整颗芯片的 SRAM 还大。 */
    bpp   = (((uint32_t)w * h) > (640U * 480U)) ? 1U : 2U;
    bytes = (uint32_t)w * h * bpp;

    if (bytes > STREAM_POOL_BYTES)
    {
      return false;                  /* 池子装不下，拒绝，保持当前档位不变 */
    }
    if ((bytes & 3U) != 0U)
    {
      return false;                  /* DCMI 按字搬，帧长必须是 4 的倍数 */
    }
  }

  CAM_DcmiDeInit();

  if (s_want_jpeg)
  {
    if (ov3660_set_pixformat(OV3660_PIXFORMAT_JPEG) != OV3660_OK)
    {
      return false;
    }
  }
  else if (ov3660_set_pixformat((bpp == 1U) ? OV3660_PIXFORMAT_GRAYSCALE
                                            : OV3660_PIXFORMAT_RGB565) != OV3660_OK)
  {
    return false;
  }

  /* set_framesize 要在 set_pixformat 之后：它会根据当前格式选 PLL 分档，
     JPEG 需要更快的 SYSCLK 才压得过来 */
  if (ov3660_set_framesize(fs) != OV3660_OK)
  {
    return false;
  }
  if (s_want_jpeg)
  {
    (void)ov3660_set_quality(s_jpeg_qs);
  }

  s_framesize   = fs;
  s_w           = w;
  s_h           = h;
  s_bpp         = bpp;
  s_format      = s_want_jpeg ? STREAM_FMT_JPEG
                              : ((bpp == 1U) ? STREAM_FMT_GRAY : STREAM_FMT_RGB565);
  s_frame_bytes = bytes;

  s_double_buffered = (bytes <= STREAM_HALF_POOL);
  s_buf[0] = s_pool;
  s_buf[1] = s_double_buffered ? (s_pool + STREAM_HALF_POOL) : s_pool;

  CAM_DcmiInitEx(s_pol_vs, s_pol_hs, s_pol_pclk, s_frame_bytes, s_want_jpeg);
  return true;
}

/**
  * @brief 处理 PC 发过来的命令
  *
  * 协议就是几条 ASCII 短行，方便用任何串口工具手敲调试：
  *
  *   R0\n      分辨率切 QQVGA 160x120
  *   R1\n      分辨率切 QVGA  320x240
  *   F<n>\n    限制帧率为 n fps，F0 = 不限
  *
  * 故意不回复任何内容：回复会混进图像流里，PC 端虽然能靠找帧头恢复，
  * 但会平白多出「重同步」计数，看着像出问题。
  * 命令有没有生效，PC 端看帧头里的宽高和统计里的 fps 就知道了。
  *
  * @retval true 分辨率被改了（调用者需要重启抓帧流程）
  */
static bool handle_commands(void)
{
  static char line[16];
  static uint8_t line_len;
  uint8_t  rx[32];
  uint32_t n;
  uint32_t i;
  bool     res_changed = false;

#ifdef STREAM_TRANSPORT_USB
  n = USBCDC_Read(rx, sizeof(rx));
#else
  /* 图传发送期间由 IRQ 把命令暂存进环形缓冲，避免 RX overrun。 */
  n = 0;
  while ((n < sizeof(rx)) && (s_uart_rx_tail != s_uart_rx_head))
  {
    rx[n++] = s_uart_rx_ring[s_uart_rx_tail];
    s_uart_rx_tail = (uint16_t)((s_uart_rx_tail + 1U) % STREAM_UART_RX_RING);
  }
#endif

  for (i = 0; i < n; i++)
  {
    char c = (char)rx[i];

    if ((c == '\n') || (c == '\r'))
    {
      if (line_len > 0U)
      {
        line[line_len] = '\0';

        if ((line[0] == 'R') && (line_len >= 2U))
        {
          /* R 后面直接跟档位序号，和 ov3660_framesize_t 的顺序一一对应 */
          ov3660_framesize_t fs = (ov3660_framesize_t)(line[1] - '0');

          if (fs != s_framesize)
          {
            if (apply_framesize(fs))
            {
              printf("  [命令] 分辨率 -> %ux%u %s，每帧 %lu 字节，%s缓冲\r\n",
                     (unsigned)s_w, (unsigned)s_h,
                     (s_bpp == 1U) ? "灰度" : "RGB565",
                     (unsigned long)s_frame_bytes,
                     s_double_buffered ? "双" : "单");
              res_changed = true;
            }
            else
            {
              printf("  [命令] 切分辨率失败（多半是 RAM 装不下这一档）\r\n");
            }
          }
        }
        else if ((line[0] == 'M') && (line_len >= 2U))
        {
          /* M0 = 传感器硬件 JPEG，M1 = 原始像素(RGB565/灰度) */
          bool want = (line[1] == '0');
          if (want != s_want_jpeg)
          {
            s_want_jpeg = want;
            if (apply_framesize(s_framesize))
            {
              printf("  [命令] 格式 -> %s\r\n", want ? "硬件 JPEG" : "原始像素");
              if (want)
              {
                /* 把 JPEG 通路相关的寄存器读回来，确认到底哪一条没生效 */
                static const uint16_t jr[] = { 0x501F, 0x4300, 0x3002, 0x3006,
                                               0x471C, 0x4713, 0x4407, 0x460B,
                                               0x460C, 0x3824, 0x3008, 0x3005 };
                uint32_t z;
                printf("  [JPEG] 寄存器回读：");
                for (z = 0; z < (sizeof(jr) / sizeof(jr[0])); z++)
                {
                  uint8_t v = 0;
                  (void)ov3660_read_reg(jr[z], &v);
                  printf("%04X=%02X ", jr[z], v);
                }
                printf("\r\n");
              }
              res_changed = true;
            }
            else
            {
              s_want_jpeg = !want;          /* 回滚 */
              printf("  [命令] 切格式失败\r\n");
            }
          }
        }
        else if ((line[0] == 'Q') && (line_len >= 2U))
        {
          /* JPEG 量化步长，十进制 8~63。越小画质越好、码流越大 */
          uint32_t qs = 0;
          uint8_t  k;
          for (k = 1U; k < line_len; k++)
          {
            if ((line[k] >= '0') && (line[k] <= '9'))
            {
              qs = (qs * 10U) + (uint32_t)(line[k] - '0');
            }
          }
          /* 上限取 48 不是 63：实测 qs=63 时编码器输出只有 68 字节，
             是个没有图像数据的空壳 JPEG，等于坏帧。48 是实测仍然正常的边界。 */
          if (qs < 8U)  { qs = 8U;  }
          if (qs > 48U) { qs = 48U; }
          s_jpeg_qs = (uint8_t)qs;
          if (s_want_jpeg)
          {
            (void)ov3660_set_quality(s_jpeg_qs);
          }
          printf("  [命令] JPEG 量化步长 -> %lu（越小越清晰）\r\n", (unsigned long)qs);
        }
        else if ((line[0] == 'V') && (line_len == 2U))
        {
          s_uart_mirror = (line[1] != '0');
        }
        else if ((line[0] == 'T') && (line_len == 2U))
        {
          s_perf_log = (line[1] != '0');
        }
#ifdef STREAM_TRANSPORT_DUAL
        else if ((line[0] == 'S') && (line_len == 2U))
        {
          s_spi_level = (unsigned)(line[1] - '0');
          CC_LinkSetSpeed(s_spi_level);
        }
#endif
        else if ((line[0] == 'P') && (line_len == 2U))
        {
          s_jpeg_pipeline = (line[1] != '0');
          if (apply_framesize(s_framesize)) res_changed = true;
        }
        else if ((line[0] == 'F') && (line_len >= 2U))
        {
          uint32_t fps = 0;
          uint8_t  k;
          for (k = 1U; k < line_len; k++)
          {
            if ((line[k] >= '0') && (line[k] <= '9'))
            {
              fps = (fps * 10U) + (uint32_t)(line[k] - '0');
            }
          }
          if (fps > 1000U) fps = 1000U;
          s_min_period_ms = (fps == 0U) ? 0U : ((1000U + fps - 1U) / fps);
          printf("[FPS] limit=%lu period_ms=%lu\r\n",
                 (unsigned long)fps, (unsigned long)s_min_period_ms);
          printf("  [命令] 帧率上限 -> %s\r\n",
                 (fps == 0U) ? "不限" : "见下");
          if (fps != 0U)
          {
            printf("         %lu fps (最小间隔 %lu ms)\r\n",
                   (unsigned long)fps, (unsigned long)s_min_period_ms);
          }
        }
        line_len = 0;
      }
    }
    else if (line_len < (sizeof(line) - 1U))
    {
      line[line_len++] = c;
    }
    else
    {
      line_len = 0;      /* 行太长，多半是撞上了噪声，丢掉重来 */
    }
  }

  return res_changed;
}

/**
  * @brief 抓不到帧时把传感器的实际状态打出来
  *
  * 用来区分两种完全不同的故障：
  *   VSYNC 不翻转        -> 传感器根本没在输出，是寄存器配置把它配停了
  *   VSYNC 翻转但尺寸不符 -> 在输出，但输出的尺寸和我们要的对不上，DMA 凑不满
  */
static void dump_sensor_state(void)
{
  static const uint16_t regs[] = { 0x3808, 0x3809, 0x380A, 0x380B,
                                   0x380C, 0x380D, 0x380E, 0x380F };
  static const char *names[] = { "X_OUT_H", "X_OUT_L", "Y_OUT_H", "Y_OUT_L",
                                 "X_TOTAL_H", "X_TOTAL_L", "Y_TOTAL_H", "Y_TOTAL_L" };
  cam_probe_t pr;
  uint8_t  v;
  uint32_t k;
  uint32_t xo = 0;
  uint32_t yo = 0;

  printf("\r\n  ---- 诊断：传感器实际输出配置 ----\r\n");
  for (k = 0; k < (sizeof(regs) / sizeof(regs[0])); k++)
  {
    v = 0;
    (void)ov3660_read_reg(regs[k], &v);
    printf("    0x%04X %-10s = 0x%02X\r\n", regs[k], names[k], v);
    if (k == 0U) { xo  = (uint32_t)v << 8; }
    if (k == 1U) { xo |= v; }
    if (k == 2U) { yo  = (uint32_t)v << 8; }
    if (k == 3U) { yo |= v; }
  }
  printf("    => 传感器实际输出 %ux%u，固件要的是 %ux%u\r\n",
         (unsigned)xo, (unsigned)yo, (unsigned)s_w, (unsigned)s_h);

  CAM_DcmiDeInit();
  CAM_ProbeSignals(300, &pr);
  printf("  ---- 诊断：同步信号（当普通 GPIO 采样 300ms）----\r\n");
  printf("    采样 %lu 次   PCLK 翻转 %lu   HREF 翻转 %lu   VSYNC 翻转 %lu\r\n",
         (unsigned long)pr.sample_count, (unsigned long)pr.pclk_edges,
         (unsigned long)pr.href_edges,   (unsigned long)pr.vsync_edges);
  if (pr.vsync_edges >= 2U)
  {
    printf("    平均每帧 HREF 脉冲约 %lu 个（轮询采样，只看数量级）\r\n",
           (unsigned long)(pr.href_edges / pr.vsync_edges));
  }
}

/* --------------------------------------------------------------- main ---- */

int main(void)
{
  uint16_t pid;
  uint32_t cur = 0;
  bool jpeg_active = false;

  BSP_Init();
  white_leds_off_init();
#ifndef STREAM_TRANSPORT_USB
  stream_uart_rx_init();
#endif

  /* 先按开机默认分辨率把 s_w/s_h/s_bpp/s_frame_bytes 和缓冲指针填好，
     后面 banner 和 pick_polarity 都要用。
     真正配传感器在下面 apply_framesize()，这里只是先算出尺寸。 */
  ov3660_get_resolution(s_framesize, &s_w, &s_h);
  s_bpp             = (((uint32_t)s_w * s_h) > (640U * 480U)) ? 1U : 2U;
  s_frame_bytes     = (uint32_t)s_w * s_h * s_bpp;
  s_double_buffered = (s_frame_bytes <= STREAM_HALF_POOL);
  s_buf[0] = s_pool;
  s_buf[1] = s_double_buffered ? (s_pool + STREAM_HALF_POOL) : s_pool;

  printf("\r\n============================================================\r\n");
  printf("  OV3660 实时图传    STM32U575 @ 160 MHz\r\n");
  printf("  开机分辨率 %ux%u %s，每帧 %u 字节，%s缓冲（运行时可改）\r\n",
         (unsigned)s_w, (unsigned)s_h, (s_bpp == 1U) ? "灰度" : "RGB565",
         (unsigned)s_frame_bytes, s_double_buffered ? "双" : "单");
  printf("  缓冲池 %u 字节，RGB565 最高到 VGA 640x480；再往上只能用灰度\r\n",
         (unsigned)STREAM_POOL_BYTES);
  printf("  图像通道：%s\r\n", STREAM_LINK_NAME);
  printf("  文字日志：ST-LINK 虚拟串口 @ %u（就是你现在看的这个口）\r\n",
         (unsigned)VCP_BAUDRATE);
  printf("============================================================\r\n");

  /* USB 要先枚举，越早开始越好，摄像头初始化那 2 秒正好拿来枚举 */
  stream_link_init();

  /* ---- 上电：3V3 -> 1V5 -> 2V8，顺序不能反 ---- */
  CAM_PowerRailsOn();
  CAM_SccbInit();
  CAM_BindSensorIo();
  CAM_XclkStart();
  CAM_PowerUp();

  /* ---- 认芯片 ---- */
  if ((ov3660_read_id(&pid) != OV3660_OK) || (pid != OV3660_CHIP_PID))
  {
    die("读不到 OV3660 芯片 ID",
        "先跑一遍验证 demo 定位问题：  make APP=camera flash");
  }
  printf("  芯片 ID = 0x%04X\r\n", pid);

  /* ---- 配格式和分辨率 ---- */
  if (ov3660_reset() != OV3660_OK)
  {
    die("载入默认寄存器表失败", "SCCB 通了但写寄存器失败，检查供电是否够稳。");
  }
  (void)ov3660_set_pixformat((s_bpp == 1U) ? OV3660_PIXFORMAT_GRAYSCALE
                                           : OV3660_PIXFORMAT_RGB565);
  (void)ov3660_set_framesize(s_framesize);

  /* AEC/AWB 收敛，不等的话头几帧会偏色或过曝 */
  HAL_Delay(1500);

  if (!pick_polarity(&s_pol_vs, &s_pol_hs, &s_pol_pclk))
  {
    dump_sensor_state();
    die("四种 DCMI 极性组合都抓不到帧",
        "看上面的诊断判断是哪一类问题。");
  }

  BSP_LedInit(LD3_GPIO_PORT, LD3_PIN);
  HAL_GPIO_WritePin(LD3_GPIO_PORT, LD3_PIN, LED_OFF_LEVEL);

#if !defined(STREAM_TRANSPORT_USB)
  /* UART links start with sensor JPEG so resets retain useful frame rates. */
  s_want_jpeg = true;
  if (!apply_framesize(s_framesize))
    die("JPEG init failed", "Check camera sensor configuration.");
#endif

#ifdef STREAM_TRANSPORT_USB
  /*
   * 等 PC 那边把这个 CDC 口打开（pyserial 一 open 就会置 DTR）。
   * 不等的话每帧都要发到超时才放弃，日志会被刷屏，也白费带宽。
   */
  if (!USBCDC_IsConfigured())
  {
    printf("  等待 USB 枚举…（没反应的话检查 CN15 的 Type-C 线是不是只有供电没有数据）\r\n");
    while (!USBCDC_IsConfigured())
    {
      HAL_Delay(100);
    }
  }
  printf("  USB 已枚举，PC 上会多出一个 COM 口。\r\n");
  if (!USBCDC_IsPortOpen())
  {
    printf("  等待 PC 打开该端口：python tools/stream_server.py COMx\r\n");
    while (!USBCDC_IsPortOpen())
    {
      HAL_Delay(300);
    }
  }
  printf("  端口已打开。\r\n");
#endif

  printf("  开始串流。\r\n\r\n");

  /*
   * 主循环：抓发重叠
   *   先启动 buffer[cur] 的抓帧，DCMI+GPDMA 在后台搬；
   *   搬完之后立刻启动 buffer[cur^1] 的下一帧；
   *   然后 CPU 才把 buffer[cur] 慢慢从串口发出去。
   *   发送期间下一帧正被 DMA 填进另一个 buffer，互不干扰。
   */
  if ((s_format != STREAM_FMT_JPEG) &&
      (CAM_CaptureStart(s_buf[cur], s_frame_bytes) != HAL_OK))
  {
    die("启动抓帧失败", "DCMI/GPDMA 初始化有问题。");
  }

  while (1)
  {
    uint32_t now;
    uint32_t len;

    /*
     * ---------------- JPEG 模式：独立的一条路径 ----------------
     * Two bounded JPEG buffers allow DCMI/GPDMA to capture the next snapshot
     * while CPU/SPI transmits the previous frame. Commands run only after
     * finishing the pending capture, before starting a new one.
     */
    if (s_format == STREAM_FMT_JPEG)
    {
      if (!jpeg_active)
      {
        if (handle_commands()) { cur = 0; continue; }
        jpeg_active = CAM_CaptureJpegStart(s_buf[cur], s_frame_bytes) == HAL_OK;
        if (!jpeg_active) continue;
      }
      len = CAM_CaptureJpegWait(STREAM_CAP_TIMEOUT);
      jpeg_active = false;
      g_stream_capture_ms = CAM_GetJpegCaptureMs();
      if (len == 0U)
      {
        /* 抓不到就每 2 秒打一行诊断（打太密会把日志刷爆） */
        static uint32_t s_last_diag;
        uint32_t t = HAL_GetTick();
        if ((uint32_t)(t - s_last_diag) > 2000U)
        {
          s_last_diag = t;
          printf("  [JPEG] 没抓到完整帧：VSYNC=%lu  开头 %02X %02X %02X %02X\r\n",
                 (unsigned long)CAM_GetVsyncCount(),
                 s_pool[0], s_pool[1], s_pool[2], s_pool[3]);
        }
        continue;
      }
      if (handle_commands()) { cur = 0; continue; }

      uint32_t send_cur = cur;
      if (s_double_buffered) {
        cur ^= 1U;
        jpeg_active = CAM_CaptureJpegStart(s_buf[cur], s_frame_bytes) == HAL_OK;
      }

      now = HAL_GetTick();
      if ((s_min_period_ms == 0U) ||
          ((uint32_t)(now - s_last_tx_ms) >= s_min_period_ms))
      {
        uint32_t send_start = HAL_GetTick();
        (void)send_frame(s_buf[send_cur], len);
        g_stream_send_ms = HAL_GetTick() - send_start;
        g_stream_jpeg_bytes = len;
        s_seq++;
        s_last_tx_ms = now;
        if (s_perf_log && !(s_seq % 16U))
          printf("[PERF] bytes=%lu capture_ms=%lu send_ms=%lu pipeline=%u mirror=%u spi=%u\r\n",
                 (unsigned long)len, (unsigned long)g_stream_capture_ms,
                 (unsigned long)g_stream_send_ms, s_double_buffered, s_uart_mirror, s_spi_level);
      }
      continue;
    }

    /* ---------------- 原始像素模式：定长，可以抓发重叠 ---------------- */
    if (CAM_CaptureWait(STREAM_CAP_TIMEOUT) != HAL_OK)
    {
      /* 偶发超时不致命：重启这一帧继续跑。
         这里不打印，否则文字会混进串流数据里 */
      (void)CAM_CaptureStart(s_buf[cur], s_frame_bytes);
      continue;
    }

    /*
     * 处理 PC 命令要放在【这里】：上一帧刚搬完、下一帧还没启动，
     * DCMI 处于空闲状态，这时候切分辨率才安全。
     * 放到抓帧进行中去改，DMA 会写到半路被改尺寸，画面直接烂掉。
     */
    if (handle_commands())
    {
      /* 分辨率/格式变了，缓冲用法也变了，重新从头开始抓。
         切成 JPEG 的话下一轮会走 JPEG 分支，这里就不用启动定长抓帧了 */
      cur = 0;
      if ((s_format != STREAM_FMT_JPEG) &&
          (CAM_CaptureStart(s_buf[cur], s_frame_bytes) != HAL_OK))
      {
        die("切分辨率后重启抓帧失败", "DCMI 重新初始化有问题。");
      }
      continue;
    }

    /*
     * 双缓冲时立刻启动下一帧，让「抓下一帧」和「发这一帧」重叠。
     * 单缓冲时【不能】这么做 —— 只有一块内存，一边发一边往里写，
     * 发出去的会是半新半旧的画面，表现为横向撕裂。
     * 所以大分辨率下只能发完再抓，帧率大约减半，这是内存换来的代价。
     */
    if (s_double_buffered)
    {
      (void)CAM_CaptureStart(s_buf[cur ^ 1U], s_frame_bytes);
    }

    /*
     * 限帧：没到间隔就不发这一帧。
     * 注意是「抓了但不发」而不是「不抓」—— 抓帧是 DMA 后台干的，不花 CPU，
     * 照抓能保证真正发出去的永远是最新的一帧，延迟不会因为限帧而变大。
     */
    now = HAL_GetTick();
    if ((s_min_period_ms == 0U) ||
        ((uint32_t)(now - s_last_tx_ms) >= s_min_period_ms))
    {
      /* seq 无论发没发成功都递增，这样 PC 端能从断层看出丢了几帧 */
      (void)send_frame(s_buf[cur], s_frame_bytes);
      s_seq++;
      s_last_tx_ms = now;

    }

    if (s_double_buffered)
    {
      cur ^= 1U;
    }
    else
    {
      /* 单缓冲：发完了才敢启动下一帧 */
      (void)CAM_CaptureStart(s_buf[0], s_frame_bytes);
    }
  }
}
