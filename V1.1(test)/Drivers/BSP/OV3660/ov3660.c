/**
  ******************************************************************************
  * @file    ov3660.c
  * @brief   OV3660 驱动实现
  *
  *   开窗 / binning / PLL 的算法移植自 espressif/esp32-camera (Apache-2.0)
  *   sensors/ov3660.c，数值与原实现一致，只是去掉了 ESP-IDF 依赖。
  ******************************************************************************
  */

#include "ov3660.h"
#include "ov3660_regs.h"
#include <stddef.h>

/* XCLK 必须和 MCO 实际输出一致，寄存器表的 PLL 参数是按这个值调的 */
#define OV3660_XCLK_HZ          16000000U

static ov3660_io_t s_io;
static uint32_t    s_pclk_hz;
static bool        s_binning;
static bool        s_binning_valid;
static bool        s_hmirror;
static bool        s_vflip;

/* 分辨率表：宽, 高（全部 4:3） */
/* 当前像素格式。set_framesize 选 PLL 分档时要区分 JPEG 和其他格式 */
static ov3660_pixformat_t s_pixformat = OV3660_PIXFORMAT_RGB565;

static const struct { uint16_t w, h; } s_res[OV3660_FRAMESIZE_MAX] = {
  {  160,  120 },   /* QQVGA    */
  {  320,  240 },   /* QVGA     */
  {  400,  300 },   /* 400x300  */
  {  480,  360 },   /* 480x360  */
  {  640,  480 },   /* VGA      */
  {  800,  600 },   /* SVGA     */
  { 1024,  768 },   /* XGA      */
  { 1600, 1200 },   /* UXGA     */
  { 2048, 1536 },   /* QXGA = 传感器满分辨率 */
};

/* 4:3 的开窗参数，取自 esp32-camera 的 ratio_table[0] */
#define RATIO_MAX_W   2048
#define RATIO_MAX_H   1536
#define RATIO_START_X 0
#define RATIO_START_Y 0
#define RATIO_END_X   2079
#define RATIO_END_Y   1547
#define RATIO_TOTAL_X 2300
#define RATIO_TOTAL_Y 1564

/* ------------------------------------------------------------------ 基础 IO */

void ov3660_bind_io(const ov3660_io_t *io)
{
  s_io = *io;
}

static int wr(uint16_t reg, uint8_t val)
{
  if (s_io.write_reg == NULL)
  {
    return OV3660_ERR_PARAM;
  }
  return s_io.write_reg(reg, val);
}

static int rd(uint16_t reg, uint8_t *val)
{
  if (s_io.read_reg == NULL)
  {
    return OV3660_ERR_PARAM;
  }
  return s_io.read_reg(reg, val);
}

static void dly(uint32_t ms)
{
  if (s_io.delay_ms != NULL)
  {
    s_io.delay_ms(ms);
  }
}

/** 只改寄存器里的某几个 bit */
static int wr_bits(uint16_t reg, uint8_t mask, bool set)
{
  uint8_t v;
  if (rd(reg, &v) != 0)
  {
    return -1;
  }
  v = set ? (uint8_t)(v | mask) : (uint8_t)(v & (uint8_t)(~mask));
  return wr(reg, v);
}

/** 写两对相邻寄存器（高字节在前），OV3660 的坐标寄存器都是这种形式 */
static int wr_addr16(uint16_t reg, uint16_t x, uint16_t y)
{
  if (wr(reg,     (uint8_t)(x >> 8))   != 0) { return -1; }
  if (wr(reg + 1U, (uint8_t)(x & 0xFFU)) != 0) { return -1; }
  if (wr(reg + 2U, (uint8_t)(y >> 8))   != 0) { return -1; }
  if (wr(reg + 3U, (uint8_t)(y & 0xFFU)) != 0) { return -1; }
  return 0;
}

/** 刷一整张寄存器表，遇到 0xFFFF 当延时，遇到 {0,0} 结束 */
static int wr_table(const ov3660_reg_t *tbl, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++)
  {
    if (tbl[i].reg == OV3660_REG_DLY)
    {
      dly(tbl[i].val);
      continue;
    }
    if ((tbl[i].reg == OV3660_REGLIST_TAIL) && (tbl[i].val == 0U))
    {
      break;
    }
    if (wr(tbl[i].reg, (uint8_t)tbl[i].val) != 0)
    {
      return -1;
    }
  }
  return 0;
}

#define WR_TABLE(t)  wr_table((t), (uint32_t)(sizeof(t) / sizeof((t)[0])))

/* ------------------------------------------------------------------ 对外 API */

int ov3660_read_id(uint16_t *pid)
{
  uint8_t h;
  uint8_t l;

  if (rd(OV3660_REG_CHIP_ID_H, &h) != 0)
  {
    return OV3660_ERR_IO;
  }
  if (rd(OV3660_REG_CHIP_ID_L, &l) != 0)
  {
    return OV3660_ERR_IO;
  }
  if (pid != NULL)
  {
    *pid = (uint16_t)(((uint16_t)h << 8) | l);
  }
  return OV3660_OK;
}

int ov3660_reset(void)
{
  /* 0x3008 bit7 = 软复位，寄存器全部回默认值 */
  if (wr(OV3660_SYSTEM_CTROL0, 0x82) != 0)
  {
    return OV3660_ERR_IO;
  }
  dly(100);

  if (WR_TABLE(ov3660_default_regs) != 0)
  {
    return OV3660_ERR_IO;
  }
  dly(100);

  s_binning_valid = false;
  s_binning = false;
  s_hmirror = false;
  s_vflip   = false;
  return OV3660_OK;
}

int ov3660_set_pixformat(ov3660_pixformat_t fmt)
{
  int ret;

  switch (fmt)
  {
    case OV3660_PIXFORMAT_RGB565:    ret = WR_TABLE(ov3660_fmt_rgb565);    break;
    case OV3660_PIXFORMAT_YUV422:    ret = WR_TABLE(ov3660_fmt_yuv422);    break;
    case OV3660_PIXFORMAT_GRAYSCALE: ret = WR_TABLE(ov3660_fmt_grayscale); break;
    case OV3660_PIXFORMAT_JPEG:      ret = WR_TABLE(ov3660_fmt_jpeg);      break;
    default:                         return OV3660_ERR_PARAM;
  }
  if (ret == 0)
  {
    s_pixformat = fmt;      /* set_framesize 要靠它选 PLL 分档 */
  }
  return (ret == 0) ? OV3660_OK : OV3660_ERR_IO;
}

int ov3660_set_quality(uint8_t qs)
{
  /*
   * 0x4407 是 JPEG 的量化步长(QS)，只有低 6 位有效。
   * ★ 数值越【小】画质越好、码流越大 ★ ——和常见的「质量 0~100」方向相反，很容易搞反。
   * esp32-camera 默认给 0x0C，范围建议 0x08(很清晰但很大) ~ 0x3F(很糊但很小)。
   */
  if (qs > 0x3FU)
  {
    qs = 0x3FU;
  }
  return (wr(0x4407U, qs) == 0) ? OV3660_OK : OV3660_ERR_IO;
}

/**
  * @brief 配 sensor 内部 PLL
  * @note  参数含义和 esp32-camera 的 set_pll() 完全一致，
  *        顺带按同样的公式把 PCLK 算出来存进 s_pclk_hz，方便上层估算带宽
  */
static int set_pll(uint8_t multiplier, uint8_t sys_div, uint8_t pre_div,
                   bool root_2x, uint8_t seld5, uint8_t pclk_div)
{
  static const int pre_div2x_map[4] = { 2, 3, 4, 6 };  /* 乘了 2 以避开浮点 */
  static const int seld52x_map[4]   = { 2, 2, 4, 5 };

  int sd;
  int vco;
  int pllclk;

  if ((multiplier > 31U) || (sys_div > 15U) || (pre_div > 3U) ||
      (pclk_div > 31U) || (seld5 > 3U))
  {
    return OV3660_ERR_PARAM;
  }

  sd     = (sys_div == 0U) ? 1 : (int)sys_div;
  vco    = (int)(OV3660_XCLK_HZ / 1000U) * (int)multiplier * (root_2x ? 2 : 1) * 2
           / pre_div2x_map[pre_div];
  pllclk = vco * 1000 * 2 / sd / seld52x_map[seld5];
  s_pclk_hz = (uint32_t)(pllclk / 2 / ((pclk_div != 0U) ? (int)pclk_div : 1));

  if (wr(OV3660_SC_PLLS_CTRL0, 0x00) != 0)                                 { return OV3660_ERR_IO; }
  if (wr(OV3660_SC_PLLS_CTRL1, (uint8_t)(multiplier & 0x1FU)) != 0)        { return OV3660_ERR_IO; }
  if (wr(OV3660_SC_PLLS_CTRL2, (uint8_t)(0x10U | (sys_div & 0x0FU))) != 0) { return OV3660_ERR_IO; }
  if (wr(OV3660_SC_PLLS_CTRL3, (uint8_t)(((uint8_t)((pre_div & 0x3U) << 4)) | seld5
                                         | (root_2x ? 0x40U : 0x00U))) != 0)
  {
    return OV3660_ERR_IO;
  }
  if (wr(OV3660_PCLK_RATIO, (uint8_t)(pclk_div & 0x1FU)) != 0)             { return OV3660_ERR_IO; }
  /* 0x22 = PCLK 手动分频生效 */
  if (wr(OV3660_VFIFO_CTRL0C, 0x22) != 0)                                  { return OV3660_ERR_IO; }
  return OV3660_OK;
}

/** binning / 镜像 / 翻转 会同时影响这几个寄存器，统一在这里刷 */
static int set_image_options(void)
{
  uint8_t reg20   = 0;
  uint8_t reg21   = 0;
  uint8_t reg4514 = 0;
  uint8_t sel     = 0;

  /*
   * ★ 0x3821 的 bit5 是【压缩使能】★
   *
   * 这一位不置，其他 JPEG 寄存器（0x501F/0x4300/0x3002/0x3006/0x471C/0x4713）
   * 全配对了也没用 —— DVP 照样吐 YUV422 原始像素。
   * 症状极隐蔽：寄存器回读一切正常，抓到的数据开头是 41 78 40 7E 这种
   * YUYV 交替，而不是 FF D8。
   */
  if (s_pixformat == OV3660_PIXFORMAT_JPEG)
  {
    reg21 |= 0x20U;
  }

  if (s_binning)
  {
    reg20 |= 0x01U;
    reg21 |= 0x01U;
    sel   |= 4U;
  }
  else
  {
    reg20 |= 0x40U;
  }
  if (s_vflip)
  {
    reg20 |= 0x06U;
    sel   |= 1U;
  }
  if (s_hmirror)
  {
    reg21 |= 0x06U;
    sel   |= 2U;
  }

  switch (sel)
  {
    case 0:  reg4514 = 0x88; break;   /* 无 binning，正常       */
    case 1:  reg4514 = 0x88; break;   /* 无 binning，垂直翻     */
    case 2:  reg4514 = 0xbb; break;   /* 无 binning，水平镜     */
    case 3:  reg4514 = 0xbb; break;   /* 无 binning，翻+镜      */
    case 4:  reg4514 = 0xaa; break;   /* binning，正常          */
    case 5:  reg4514 = 0xbb; break;   /* binning，垂直翻        */
    case 6:  reg4514 = 0xbb; break;   /* binning，水平镜        */
    default: reg4514 = 0xaa; break;   /* binning，翻+镜         */
  }

  if (wr(OV3660_TIMING_TC_REG20, reg20) != 0) { return OV3660_ERR_IO; }
  if (wr(OV3660_TIMING_TC_REG21, reg21) != 0) { return OV3660_ERR_IO; }
  if (wr(0x4514, reg4514) != 0)               { return OV3660_ERR_IO; }

  if (s_binning)
  {
    if (wr(0x4520, 0x0b) != 0)             { return OV3660_ERR_IO; }
    if (wr(OV3660_X_INCREMENT, 0x31) != 0) { return OV3660_ERR_IO; }
    if (wr(OV3660_Y_INCREMENT, 0x31) != 0) { return OV3660_ERR_IO; }
  }
  else
  {
    if (wr(0x4520, 0xb0) != 0)             { return OV3660_ERR_IO; }
    if (wr(OV3660_X_INCREMENT, 0x11) != 0) { return OV3660_ERR_IO; }
    if (wr(OV3660_Y_INCREMENT, 0x11) != 0) { return OV3660_ERR_IO; }
  }
  return OV3660_OK;
}

int ov3660_set_framesize(ov3660_framesize_t size)
{
  uint16_t w;
  uint16_t h;
  bool scale;

  if (size >= OV3660_FRAMESIZE_MAX)
  {
    return OV3660_ERR_PARAM;
  }
  w = s_res[size].w;
  h = s_res[size].h;

  s_binning       = (w <= (RATIO_MAX_W / 2)) && (h <= (RATIO_MAX_H / 2));
  s_binning_valid = true;
  scale = !(((w == RATIO_MAX_W) && (h == RATIO_MAX_H)) ||
            ((w == (RATIO_MAX_W / 2)) && (h == (RATIO_MAX_H / 2))));

  if (wr_addr16(OV3660_X_ADDR_ST_H,     RATIO_START_X, RATIO_START_Y) != 0) { return OV3660_ERR_IO; }
  if (wr_addr16(OV3660_X_ADDR_END_H,    RATIO_END_X,   RATIO_END_Y)   != 0) { return OV3660_ERR_IO; }
  if (wr_addr16(OV3660_X_OUTPUT_SIZE_H, w,             h)             != 0) { return OV3660_ERR_IO; }

  if (s_binning)
  {
    if (wr_addr16(OV3660_X_TOTAL_SIZE_H, RATIO_TOTAL_X, (RATIO_TOTAL_Y / 2) + 1) != 0)
    {
      return OV3660_ERR_IO;
    }
    if (wr_addr16(OV3660_X_OFFSET_H, 8, 2) != 0) { return OV3660_ERR_IO; }
  }
  else
  {
    if (wr_addr16(OV3660_X_TOTAL_SIZE_H, RATIO_TOTAL_X, RATIO_TOTAL_Y) != 0)
    {
      return OV3660_ERR_IO;
    }
    if (wr_addr16(OV3660_X_OFFSET_H, 16, 6) != 0) { return OV3660_ERR_IO; }
  }

  if (wr_bits(OV3660_ISP_CONTROL_01, 0x20, scale) != 0)
  {
    return OV3660_ERR_IO;
  }
  if (set_image_options() != OV3660_OK)
  {
    return OV3660_ERR_IO;
  }

  /*
   * PLL 参数按 esp32-camera 的分档来，都是针对 XCLK=16MHz 调过的，PCLK 统一 8MHz，
   * 差别只在传感器内部 SYSCLK，也就决定了它能跑多少帧。
   *
   * ★ 这里按【像素总数】判断，不是按枚举序号 ★
   *   原来写的是 `size > OV3660_FRAMESIZE_VGA`，枚举中间一插新档位就全错位了，
   *   而且错得很隐蔽：PLL 配错不会报错，只是帧率不对或者干脆不出图。
   *   esp32-camera 的分界点是 HVGA(480x320)=153600 像素和 QVGA(320x240)=76800 像素，
   *   换算成像素数就和枚举顺序解耦了。
   */
  {
    uint32_t pixels = (uint32_t)w * (uint32_t)h;

    if (s_pixformat == OV3660_PIXFORMAT_JPEG)
    {
      /*
       * JPEG 模式单独一档：40MHz SYSCLK / 10MHz PCLK。
       * 硬件压缩要跑得比出原始像素快得多，用原始格式那几档 PLL 会压不过来，
       * 表现是码流断断续续或者直接不出 EOI。参数取自 esp32-camera 的 xclk=16MHz 分支。
       */
      return set_pll(24, 1, 3, false, 0, 8);
    }

    if (pixels > (480U * 320U))
    {
      return set_pll(4, 1, 0, false, 2, 2);   /* SYSCLK  8MHz，约 4.4 fps */
    }
    else if (pixels >= (320U * 240U))
    {
      return set_pll(8, 1, 0, false, 2, 4);   /* SYSCLK 16MHz，约 10.3 fps */
    }
    else
    {
      return set_pll(8, 1, 0, false, 0, 8);   /* SYSCLK 32MHz，约 17.8 fps */
    }
  }
}

int ov3660_set_colorbar(bool enable)
{
  /* 0x503D 的 bit7(测试图使能) + bit6(滚动彩条)。
     镜头/光路有问题时用它单独验证「传感器输出 -> DCMI -> 内存」这条通路。 */
  return (wr_bits(OV3660_PRE_ISP_TEST_SETTING_1, OV3660_TEST_COLOR_BAR, enable) == 0)
         ? OV3660_OK : OV3660_ERR_IO;
}

int ov3660_set_mirror_flip(bool hmirror, bool vflip)
{
  s_hmirror = hmirror;
  s_vflip   = vflip;
  if (!s_binning_valid)
  {
    s_binning = false;
  }
  return set_image_options();
}

void ov3660_get_resolution(ov3660_framesize_t size, uint16_t *w, uint16_t *h)
{
  ov3660_framesize_t s = (size >= OV3660_FRAMESIZE_MAX) ? OV3660_FRAMESIZE_QVGA : size;

  if (w != NULL) { *w = s_res[s].w; }
  if (h != NULL) { *h = s_res[s].h; }
}

uint32_t ov3660_get_pclk_hz(void)
{
  return s_pclk_hz;
}

int ov3660_write_reg(uint16_t reg, uint8_t val)
{
  return (wr(reg, val) == 0) ? OV3660_OK : OV3660_ERR_IO;
}

int ov3660_read_reg(uint16_t reg, uint8_t *val)
{
  return (rd(reg, val) == 0) ? OV3660_OK : OV3660_ERR_IO;
}
