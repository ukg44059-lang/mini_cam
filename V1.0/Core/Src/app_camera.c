/**
  ******************************************************************************
  * @file    app_camera.c
  * @brief   OV3660 摄像头验证 demo（分阶段点亮，每一步都能单独判断对错）
  *
  *   阶段 0  上电+自检     按序打开底板 LDO，再量四条控制线区分「没供电/没接线/短路」
  *   阶段 0.5 短路扫描     逐根拉低数据线，找出连在一起的引脚对
  *   阶段 1  SCCB 通信      读芯片 ID，应为 0x3660
  *   阶段 2  寄存器读写     写一个值再读回，验证 SCCB 双向都通
  *   阶段 3  载入配置       软复位 + 197 条默认表 + RGB565 + QVGA
  *   阶段 4  信号探测       把 VSYNC/HREF/PCLK 当 GPIO 采样，看有没有在动
  *   阶段 5  DCMI 抓帧      抓一帧 QVGA RGB565，极性自动试探
  *   阶段 6  画面统计       算亮度均值/最值，判断是不是全黑/全白/花屏
  *   阶段 7  导出图像       按 B1 键把整帧从串口吐出去，PC 端存成 PNG
  *
  *   任何一步失败都会停下来并打印可能的原因，不会继续往下跑。
  ******************************************************************************
  */

#include "bsp.h"
#include "camera_port.h"
#include "ov3660.h"
#include <string.h>

/* ------------------------------------------------------------- 配置 ------- */
#define CAM_FRAMESIZE       OV3660_FRAMESIZE_QVGA
#define CAM_BYTES_PER_PIXEL 2U                       /* RGB565 */
#define CAM_MAX_W           320U
#define CAM_MAX_H           240U
#define CAM_BUF_BYTES       (CAM_MAX_W * CAM_MAX_H * CAM_BYTES_PER_PIXEL)  /* 153600 */

/* DMA 目标缓冲，必须 4 字节对齐 */
static uint8_t s_frame[CAM_BUF_BYTES] __attribute__((aligned(4)));

/* DCMI 极性：0x4740 默认写的是 0x21 -> PCLK 高有效(上升沿采样)、VSYNC 高有效。
   但不同批次模组可能不一样，阶段 5 会自动两种都试。 */
static bool s_pclk_rising = true;

static uint16_t s_w, s_h;
static uint32_t s_frame_bytes;

/* 11 根数据/同步线。板上它们【没有】上拉，全靠模组驱动。
   FPC 脚号按本板网表 Netlist_Schematic2_2026-09-03-02.tel；
   sensor 那一列是模组内部接的 OV3660 引脚（模组只引出 D[9:2]）。 */
typedef struct
{
  const char   *name;
  GPIO_TypeDef *port;
  uint16_t      pin;
  const char   *fpc;
} cam_dataline_t;

static const cam_dataline_t s_datalines[] = {
  { "PCLK  (PA6) ", GPIOA, GPIO_PIN_6,  "17" },
  { "VSYNC (PB7) ", GPIOB, GPIO_PIN_7,  " 7" },
  { "HREF  (PA4) ", GPIOA, GPIO_PIN_4,  " 9" },
  { "D7    (PE6) ", GPIOE, GPIO_PIN_6,  "12" },
  { "D6    (PB8) ", GPIOB, GPIO_PIN_8,  "14" },
  { "D5    (PB6) ", GPIOB, GPIO_PIN_6,  "16" },
  { "D4    (PE4) ", GPIOE, GPIO_PIN_4,  "18" },
  { "D3    (PC9) ", GPIOC, GPIO_PIN_9,  "20" },
  { "D2    (PC8) ", GPIOC, GPIO_PIN_8,  "22" },
  { "D1    (PC7) ", GPIOC, GPIO_PIN_7,  "21" },
  { "D0    (PC6) ", GPIOC, GPIO_PIN_6,  "19" },
};
#define CAM_NDATALINES  (sizeof(s_datalines) / sizeof(s_datalines[0]))

/* 做「模组在不在」探测用的线：11 根数据/同步线 + SCCB 两根 + 两个控制脚。
   数据线上板子没有上拉，正好可以拿内部上拉当探针。 */
static const cam_dataline_t s_esdlines[] = {
  { "PCLK  (PA6) ", GPIOA, GPIO_PIN_6,  "17" },
  { "VSYNC (PB7) ", GPIOB, GPIO_PIN_7,  " 7" },
  { "HREF  (PA4) ", GPIOA, GPIO_PIN_4,  " 9" },
  { "D7    (PE6) ", GPIOE, GPIO_PIN_6,  "12" },
  { "D6    (PB8) ", GPIOB, GPIO_PIN_8,  "14" },
  { "D5    (PB6) ", GPIOB, GPIO_PIN_6,  "16" },
  { "D4    (PE4) ", GPIOE, GPIO_PIN_4,  "18" },
  { "D3    (PC9) ", GPIOC, GPIO_PIN_9,  "20" },
  { "D2    (PC8) ", GPIOC, GPIO_PIN_8,  "22" },
  { "D1    (PC7) ", GPIOC, GPIO_PIN_7,  "21" },
  { "D0    (PC6) ", GPIOC, GPIO_PIN_6,  "19" },
  { "SCL   (PC0) ", GPIOC, GPIO_PIN_0,  " 5" },
  { "SDA   (PC1) ", GPIOC, GPIO_PIN_1,  " 3" },
  { "RESETB(PA2) ", GPIOA, GPIO_PIN_2,  " 6" },
  { "PWDN  (PA1) ", GPIOA, GPIO_PIN_1,  " 8" },
};
#define CAM_NESDLINES   (sizeof(s_esdlines) / sizeof(s_esdlines[0]))

static void banner(const char *stage)
{
  printf("\r\n------------------------------------------------------------\r\n");
  printf("  %s\r\n", stage);
  printf("------------------------------------------------------------\r\n");
}

static void fail(const char *what, const char *hint)
{
  printf("\r\n[失败] %s\r\n", what);
  printf("%s\r\n", hint);
  printf("\r\nLED6 常亮（本板是低电平点亮），程序停在这里。\r\n");

  BSP_LedInit(LD3_GPIO_PORT, LD3_PIN);
  HAL_GPIO_WritePin(LD3_GPIO_PORT, LD3_PIN, LED_ON_LEVEL);
  while (1)
  {
    __NOP();
  }
}

/* ---------------------------------------------------- 阶段 0：电气自检 --- */

/*
 * ★ 先回答一个最基本的问题：FPC 排线 + 模组到底接上了没有 ★
 *
 * 用模组自己的 ESD 二极管当探针，不需要万用表：
 *   OV3660 每个数字脚内部都有一只 ESD 二极管接着 DOVDD。
 *   把 2V8 关掉 -> DOVDD = 0V，然后在 MCU 侧打开【内部上拉】（约 40k 到 3.3V），
 *   那只二极管就会导通，把这条线钳在 0.6V 左右 -> 引脚读回 0。
 *   如果这头根本没接东西，内部上拉直接把它拉到 3.3V -> 读回 1。
 *
 *   两个前提都不能少：
 *     - 2V8 必须是关的（开着的话二极管反偏，不导通，什么也测不出来）
 *     - 必须开内部上拉（数据线上板子本来就没有上拉）
 *
 *   所以「读到 0」= 那一头确实挂着一个钳位器件 = 模组接上了。
 */
static uint32_t stage_module_present(void)
{
  GPIO_InitTypeDef gi = {0};
  uint32_t i;
  uint32_t clamped = 0;
  bool     v28_on  = false;      /* 关掉之后 2V8 是否仍然有电（H26 跳错侧） */

  /* 先把两路摄像头电源都压到关（外部还有 4.7k 下拉，双保险）。
     ★ 下面这些探测要碰 GPIOA(PA4/PA6)、GPIOB(PB6/7/8)、GPIOC(PC0/1/6/7/8/9/13/14)、
       GPIOE(PE4/PE6)，所以四个口的时钟都要开 —— 之前漏了 GPIOC/GPIOA，
       导致 PC 那 8 根线从来没被配成输入+上拉，读回来的全是悬空值，探针结果错得离谱。 */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  gi.Mode  = GPIO_MODE_OUTPUT_PP;
  gi.Pull  = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  gi.Pin = CAM_EN_1V5_PIN;
  HAL_GPIO_Init(CAM_EN_1V5_PORT, &gi);
  gi.Pin = CAM_EN_2V8_PIN;
  HAL_GPIO_Init(CAM_EN_2V8_PORT, &gi);
  HAL_GPIO_WritePin(CAM_EN_2V8_PORT, CAM_EN_2V8_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(CAM_EN_1V5_PORT, CAM_EN_1V5_PIN, GPIO_PIN_RESET);
  HAL_Delay(200);   /* 等 2V8 轨充分放电（R46/R47 没焊，放电只靠 R44/R45，可能偏慢） */

  /*
   * ★ 先验证前提：2V8 到底有没有被关掉 ★
   *
   * ESD 探针只有在「DOVDD 真的是 0V」时才成立。如果板上 H26 跳在 3V3 那侧，
   * U4 的 EN 就硬接 3V3 —— 固件把 PB1 拉低也关不掉 2V8，二极管始终反偏，
   * 于是不管模组插没插都会读成"这一头是空的"，得出完全错误的结论。
   *
   * SCL(PC0) 上挂着 4.7k 到 2V8，正好可以当 2V8 的电压探针：
   *   加内部下拉（约 40k）还读到高 -> 2V8 还在
   *   读到低                      -> 2V8 确实没了
   */
  {
    GPIO_InitTypeDef g2 = {0};

    g2.Pin   = CAM_SCL_PIN;
    g2.Mode  = GPIO_MODE_INPUT;
    g2.Pull  = GPIO_PULLDOWN;
    g2.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(CAM_SCCB_PORT, &g2);
    HAL_Delay(2);
    v28_on = (HAL_GPIO_ReadPin(CAM_SCCB_PORT, CAM_SCL_PIN) == GPIO_PIN_SET);
    HAL_GPIO_DeInit(CAM_SCCB_PORT, CAM_SCL_PIN);

    banner("阶段 -1：模组在不在（ESD 二极管探针）");
    printf("  [前提检查] 把 2V8 关掉之后，用 SCL 上的 4.7k 当探针复测：\r\n");
    if (v28_on)
    {
      printf("     SCL 加内部下拉仍读高 -> ★ 2V8 还在！固件关不掉它 ★\r\n");
      printf("     说明 H26(2.8V_EN) 跳在【3V3 那侧】= 常开，EN 被硬接 3V3。\r\n");
      printf("     ⚠ 那么下面的 ESD 探针【无效】：二极管一直反偏，\r\n");
      printf("       不管模组插没插都会读成「空的」。请先把 H26 跳到 MCU 侧再来。\r\n\r\n");
    }
    else
    {
      printf("     SCL 加内部下拉读到低 -> 2V8 确实被关断了，探针前提成立 ✓\r\n\r\n");
    }
  }

  /* 全部配成输入 + 内部上拉 */
  gi.Mode  = GPIO_MODE_INPUT;
  gi.Pull  = GPIO_PULLUP;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  for (i = 0; i < CAM_NESDLINES; i++)
  {
    gi.Pin = s_esdlines[i].pin;
    HAL_GPIO_Init(s_esdlines[i].port, &gi);
  }
  HAL_Delay(5);

  printf("\r\n  ---- ESD 二极管探针结果 ----\r\n");
  printf("  判据：这根线被钳在低 = 那一头挂着模组；被拉高 = 那一头是空的\r\n\r\n");
  for (i = 0; i < CAM_NESDLINES; i++)
  {
    GPIO_PinState s = HAL_GPIO_ReadPin(s_esdlines[i].port, s_esdlines[i].pin);
    bool is_low = (s == GPIO_PIN_RESET);

    printf("    %s FPC第%s脚  %s\r\n", s_esdlines[i].name, s_esdlines[i].fpc,
           is_low ? "被钳在低  <- 这一头挂着模组" : "被拉高    <- 这一头是空的");
    if (is_low) { clamped++; }

    HAL_GPIO_DeInit(s_esdlines[i].port, s_esdlines[i].pin);
  }

  printf("\r\n  被钳住的线：%lu / %lu\r\n", (unsigned long)clamped, (unsigned long)CAM_NESDLINES);

  if (clamped == 0U)
  {
    if (v28_on)
    {
      printf("  => 【结论无效】2V8 根本没关掉（见上面的前提检查），\r\n");
      printf("     二极管一直反偏，所以 15 根线必然全读高。\r\n");
      printf("     ★ 先把 H26 从 3V3 侧改跳到 MCU 侧（PB1），再重跑自检。★\r\n");
    }
    else
    {
      printf("  => 【一根都没钳住】：FPC 排线/模组没插，或者插反了、没插到底。\r\n");
      printf("     前提检查已确认 2V8 确实被关断，所以这个结论是可信的。\r\n");
    }
  }
  else if (clamped >= (CAM_NESDLINES * 2U / 3U))
  {
    printf("  => 绝大多数线都被钳住：FPC 排线 + 模组【确实接在座子上】。\r\n");
    printf("     那问题不在「插没插」，往供电(DVDD 1.5V)和时序上查。\r\n");
  }
  else
  {
    printf("  => 只有一部分线被钳住：接触不良，或者排线只压上了一侧。\r\n");
    printf("     重点查 FPC 座有没有压紧、金手指有没有脏。\r\n");
  }

  return clamped;
}

/*
 * 在碰 I2C 之前，先纯电气地量一下四条「靠外部 2.8V 上拉」的线。
 * 这一步不需要传感器配合，能把「没供电 / 没接线 / 短路」直接区分开，
 * 省得 SCCB 一 NACK 就只能对着一张猜测清单挨个试。
 *
 * 判据（见 CAM_CheckLine）：
 *   加内部下拉仍读到高      -> 外面有强上拉，2.8V 到位
 *   加内部下拉低、内部上拉高 -> 悬空，没有外部上拉
 *   两次都读低              -> 对地短路
 */
static void stage_electrical(void)
{
  static const struct {
    const char   *name;
    GPIO_TypeDef *port;
    uint16_t      pin;
    const char   *where;
  } lines[] = {
    { "SCL    (PC0) ", GPIOC, GPIO_PIN_0,  "FPC 第5脚,  4.7k 上拉到 2V8 (R45)" },
    { "SDA    (PC1) ", GPIOC, GPIO_PIN_1,  "FPC 第3脚,  4.7k 上拉到 2V8 (R44)" },
    { "RESETB (PA2) ", GPIOA, GPIO_PIN_2,  "FPC 第6脚,  4.7k 上拉到 2V8 (R46)" },
    { "PWDN   (PA1) ", GPIOA, GPIO_PIN_1,  "FPC 第8脚,  4.7k 上拉到 2V8 (R47)" },
  };
  uint32_t floating = 0;
  uint32_t shorted  = 0;

  banner("阶段 0：摄像头电源 + 电气自检");

  /* 先在最干净的状态下回答「模组到底插上了没有」（两路电源都关着做） */
  (void)stage_module_present();

  /* 本板两路 LDO 的使能脚都带 4.7k 下拉，上电默认全关，必须 MCU 拉高。
     顺序固定为 1V5(DVDD) -> 2V8(AVDD/DOVDD)，反了会把 OV3660 闩锁烧掉。
     MCU 自己的 3V3 由板上 USB-C -> H21 -> U12 -> H22 提供，固件管不着。 */
  printf("  按 1V5 -> 2V8 顺序打开摄像头 LDO (PE8 / PB1)...\r\n");
  CAM_PowerRailsOn();
  printf("  电源已使能，开始量线\r\n\r\n");

  for (uint32_t i = 0; i < (sizeof(lines) / sizeof(lines[0])); i++)
  {
    cam_line_state_t st = CAM_CheckLine(lines[i].port, lines[i].pin);

    printf("  %s %s   %s\r\n", lines[i].name, CAM_LineStateStr(st), lines[i].where);

    if (st == CAM_LINE_FLOATING)   { floating++; }
    if (st == CAM_LINE_SHORT_GND)  { shorted++;  }
  }

  if (shorted > 0)
  {
    fail("有信号线被拉死在地上",
         "这四条线都靠外部上拉到 2.8V，读到对地短路只有两种可能：\r\n"
         "  1. 走线/焊点短路 —— 0.5mm 的 FPC 座最容易连锡\r\n"
         "  2. 摄像头模组已损坏，内部把引脚短到地了\r\n"
         "     （之前烧到 80°C 的那颗模组很可能就是这种情况）\r\n"
         "先把模组拔掉再跑一次：拔掉后变成「悬空」= 模组坏了；\r\n"
         "仍然短路 = 板子上短路。");
  }

  if (floating == 4U)
  {
    fail("四条线全部悬空 —— 2V8 没有电",
         "这四条线都靠 4.7k 上拉到 2V8，全悬空说明这条轨没建立。\r\n"
         "固件已经把 PE8(1.5V_EN) / PB1(2.8V_EN) 拉高了，所以重点查跳线和输入电源：\r\n"
         "\r\n"
         "  1. 板子 3V3 有吗？ USB-C 插了吗？ H21 跳线插了吗（5V -> 5V_ORI）？\r\n"
         "     H22 插了吗（3V3_ORI -> 3V3）？ LED5 亮不亮就是这条轨的判据。\r\n"
         "  2. ★ 两道串接跳线 ★ 不插的话量 LDO 输出正常、但负载完全没电：\r\n"
         "       H23: 2V8_ORI -> 2V8   （SCL/SDA/RESETB/PWDN 的上拉就挂在这条）\r\n"
         "       H25: 1V5_ORI -> 1V5\r\n"
         "  3. 使能跳线 H26(2.8V) / H28(1.5V) 的位置：\r\n"
         "       接 3V3 那侧 = 常开；接 MCU 那侧 = PB1/PE8 控制（固件已拉高，都行）\r\n"
         "  4. 量 U4(TPS7A0228) 的 EN 脚(PB1) 有没有 3.3V、OUT 脚有没有 2.8V\r\n"
         "     U8(TPS7A0215) 的 EN 脚(PE8) 有没有 3.3V、OUT 脚有没有 1.5V\r\n"
         "\r\n"
         "注意：量 LDO 输出脚正常 ≠ 负载有电，中间还隔着串接跳线。\r\n"
         "      要量就直接量 FPC 第10脚(应 1.5V) 和第11脚(应 2.8V)。");
  }

  if (floating > 0)
  {
    printf("\r\n  [警告] 有 %lu 条线悬空\r\n", (unsigned long)floating);
  }
  else
  {
    printf("\r\n  四条线都有外部上拉 -> 板上 2V8 到位、MCU 引脚也对上了。\r\n");
    printf("  注意：这四条上拉在【板子上】，所以这一项通过只能证明 2V8 没问题，\r\n");
    printf("        并不能证明摄像头模组真的插好了。下面这项才是查模组的。\r\n");
  }

  /* ---- 模组在不在：数据/同步线板上没有上拉，全靠模组自己驱动 ---- */
  {
    const cam_dataline_t *dl = s_datalines;
    uint32_t n = CAM_NDATALINES;
    uint32_t driven = 0;

    printf("\r\n  ---- 摄像头模组在位检测 ----\r\n");
    printf("  这 11 根线板上【没有】上拉，只有模组插好且上电时才会被驱动：\r\n");

    for (uint32_t i = 0; i < n; i++)
    {
      cam_line_state_t st = CAM_CheckLine(dl[i].port, dl[i].pin);
      const char *tag = (st == CAM_LINE_FLOATING) ? "悬空" :
                        (st == CAM_LINE_SHORT_GND) ? "被拉低" : "被拉高";

      printf("    %s FPC第%s脚  %s\r\n", dl[i].name, dl[i].fpc, tag);
      if (st != CAM_LINE_FLOATING) { driven++; }
    }

    printf("\r\n  被驱动的线：%lu / %lu\r\n", (unsigned long)driven, (unsigned long)n);

    /* ---- FPC 排线到底插上没有：比对地电容 ----
       只在「一根都没被驱动」时才有意义。模组正常工作时会主动驱动这些线，
       内部上拉根本拉不高，测出来全是超时值，反而误导。 */
    if (driven == 0U)
    {
      /* 基准脚：PC10/PC11/PC12 在本板网表里没有出现（没走线），正好当"什么都没接"的基准。
         摄像头那 11 根脚则是 排针 -> PCB走线 -> FPC座 -> 排线 -> 模组焊盘，
         排线真插上的话电容明显更大，充电周期数会高出一截。 */
      static const struct { const char *name; GPIO_TypeDef *port; uint16_t pin; } ref[] = {
        { "PC10", GPIOC, GPIO_PIN_10 },
        { "PC11", GPIOC, GPIO_PIN_11 },
        { "PC12", GPIOC, GPIO_PIN_12 },
      };
      uint32_t rsum = 0, dsum = 0;
      uint32_t rn = sizeof(ref) / sizeof(ref[0]);

      printf("\r\n  ---- FPC 排线在位判断（测对地电容 / 内部上拉充电周期数）----\r\n");
      printf("  基准脚（只到排针，后面没接东西）：\r\n");
      for (uint32_t i = 0; i < rn; i++)
      {
        uint32_t c = CAM_MeasureLineRC(ref[i].port, ref[i].pin);
        printf("    %s  %lu 周期\r\n", ref[i].name, (unsigned long)c);
        rsum += c;
      }

      printf("  摄像头数据线（排针 -> 走线 -> FPC座 -> 排线 -> 模组）：\r\n");
      for (uint32_t i = 0; i < n; i++)
      {
        uint32_t c = CAM_MeasureLineRC(dl[i].port, dl[i].pin);
        printf("    %s FPC第%s脚  %lu 周期\r\n",
               dl[i].name, dl[i].fpc, (unsigned long)c);
        dsum += c;
      }

      {
        uint32_t ravg = rsum / rn;
        uint32_t davg = dsum / n;

        printf("\r\n  基准平均 %lu 周期，数据线平均 %lu 周期\r\n",
               (unsigned long)ravg, (unsigned long)davg);
        if (ravg == 0U)
        {
          printf("  [无法判断] 基准脚测出 0，DWT 计数器可能没跑起来\r\n");
        }
        else if (davg > (ravg + (ravg / 2U)))
        {
          printf("  => 数据线电容明显更大，FPC 排线【应该是插上了】。\r\n");
          printf("     那问题就在供电上：重点量 FPC 第10脚的 DVDD 1.5V。\r\n");
        }
        else
        {
          printf("  => 数据线和空脚差不多，FPC 排线【很可能没插上/没接触】。\r\n");
          printf("     重点查排线金手指朝向、有没有插到底、翻盖压紧没有。\r\n");
        }
      }
    }

    if (driven == 0U)
    {
      printf("\r\n  [重要] 11 根线全部悬空 —— 摄像头模组这一侧完全没有反应。\r\n");
      printf("         板上 2V8 是好的（上面刚验过），所以问题在模组或 FPC 上：\r\n");
      printf("         1. ★ DVDD 1.5V ★ 量 FPC 第10脚，应该是 1.5V。\r\n");
      printf("            H25 跳线不插的话，量 U8 输出正常但 FPC 第10脚是 0V，\r\n");
      printf("            数字核心不上电，SCCB 永远不会应答。\r\n");
      printf("         2. FPC 排线的金手指朝向 / 座子接触面（上接 or 下接）是否插反\r\n");
      printf("         3. FPC 排线没插到底，翻盖没压紧\r\n");
      printf("         （下面阶段 1 还会再扫一遍 I2C 总线确认）\r\n");
    }
  }
}

/* -------------------------------------------- 阶段 0.5：数据线短路扫描 --- */

/*
 * 逐根把数据线拉低，看其它线跟不跟着走，找出连在一起的引脚对。
 *
 * ★ 前提：2.8V 必须还开着，同时让传感器进掉电+复位把输出转成高阻。★
 *   - 传感器如果在正常输出，读到的是图像数据，测不出短路；
 *   - 如果把 2.8V 也关掉，模组每个脚到 DOVDD(此时0V) 的 ESD 二极管
 *     会把内部上拉钳在 0.6V，所有线都读成 0，同样测不出来。
 *
 * FPC 是 0.5mm 间距，相邻脚连锡是最常见的失效，而且症状很隐蔽：
 * 图像结构完全正常，只是颜色不对（两根数据线合并 = 某几个颜色 bit 恒等）。
 */
static void stage_shorts(void)
{
  uint8_t  low_at_baseline[CAM_NDATALINES];
  uint32_t shorts = 0;
  uint32_t stuck  = 0;
  GPIO_InitTypeDef gi = {0};

  banner("阶段 0.5：数据线短路扫描");

  /* 让传感器停止驱动，但保持 2.8V 供电 */
  CAM_SetPwdn(true);      /* PWDN 释放 -> 上拉到 2.8V -> 掉电 */
  CAM_SetReset(false);    /* RESETB 拉低 -> 复位          */
  HAL_Delay(200);

  /* 全部配成输入 + 内部上拉 */
  gi.Mode  = GPIO_MODE_INPUT;
  gi.Pull  = GPIO_PULLUP;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  for (uint32_t i = 0; i < CAM_NDATALINES; i++)
  {
    gi.Pin = s_datalines[i].pin;
    HAL_GPIO_Init(s_datalines[i].port, &gi);
  }
  HAL_Delay(5);

  /* 基线：都该被上拉成高。读到低的说明这根线被什么东西拉死了 */
  for (uint32_t i = 0; i < CAM_NDATALINES; i++)
  {
    low_at_baseline[i] =
      (HAL_GPIO_ReadPin(s_datalines[i].port, s_datalines[i].pin) == GPIO_PIN_RESET) ? 1U : 0U;
    if (low_at_baseline[i] != 0U)
    {
      printf("  [异常] %s FPC第%s脚 加了上拉仍是低电平 —— 对地短路或器件损坏\r\n",
             s_datalines[i].name, s_datalines[i].fpc);
      stuck++;
    }
  }
  if (stuck == 0U)
  {
    printf("  基线正常：11 根线加上拉后全部为高\r\n");
  }

  /* 逐根拉低，看谁跟着走 */
  for (uint32_t i = 0; i < CAM_NDATALINES; i++)
  {
    if (low_at_baseline[i] != 0U)
    {
      continue;                       /* 本来就是低的，测不出信息 */
    }

    /* 把第 i 根改成推挽输出并拉低 */
    HAL_GPIO_WritePin(s_datalines[i].port, s_datalines[i].pin, GPIO_PIN_RESET);
    gi.Pin  = s_datalines[i].pin;
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(s_datalines[i].port, &gi);
    HAL_Delay(2);

    for (uint32_t j = 0; j < CAM_NDATALINES; j++)
    {
      if ((j == i) || (low_at_baseline[j] != 0U))
      {
        continue;
      }
      /* 第 j 根有内部上拉，却跟着变低 -> 和第 i 根连在一起了 */
      if (HAL_GPIO_ReadPin(s_datalines[j].port, s_datalines[j].pin) == GPIO_PIN_RESET)
      {
        if (j > i)                    /* 每对只报一次 */
        {
          printf("  [短路] %s FPC第%s脚  <==>  %s FPC第%s脚\r\n",
                 s_datalines[i].name, s_datalines[i].fpc,
                 s_datalines[j].name, s_datalines[j].fpc);
          shorts++;
        }
      }
    }

    /* 恢复成输入 + 上拉 */
    gi.Pin  = s_datalines[i].pin;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(s_datalines[i].port, &gi);
    HAL_Delay(2);
  }

  /* 引脚交还，恢复传感器 */
  for (uint32_t i = 0; i < CAM_NDATALINES; i++)
  {
    HAL_GPIO_DeInit(s_datalines[i].port, s_datalines[i].pin);
  }
  CAM_SetPwdn(false);
  HAL_Delay(5);
  CAM_SetReset(true);
  HAL_Delay(50);

  if (shorts == 0U)
  {
    printf("\r\n  没有发现短路，11 根数据线互相独立。\r\n");
  }
  else
  {
    printf("\r\n  [严重] 发现 %lu 对短路的数据线。\r\n", (unsigned long)shorts);
    printf("  症状会是：图像结构完全正常，但颜色不对（合并的两根 = 某几个颜色 bit 恒等）。\r\n");
    printf("  FPC 是 0.5mm 间距，相邻脚连锡最常见。定位方法：\r\n");
    printf("    1. 拔掉 FPC 排线再跑一次 —— 还短 = 底板上连锡；不短了 = 排线或模组\r\n");
    printf("    2. 放大镜看 FPC 座焊脚，用助焊剂 + 吸锡带清一下\r\n");
    printf("    3. 万用表蜂鸣档直接量那两个焊脚\r\n");
    printf("\r\n  短路不影响后面的流程，会继续跑，但图像颜色是错的。\r\n");
  }
}

/* ---------------------------------------------------- 阶段 1 / 2：SCCB ---- */

static void stage_sccb(void)
{
  uint16_t pid = 0;
  uint8_t  v, back;
  int      ret;

  banner("阶段 1：I2C 总线扫描 + 读芯片 ID");

  {
    uint32_t edges = CAM_XclkEdgeCount(1000U);
    printf("  现场电平：SCL=%u SDA=%u RESETB=%u PWDN=%u\r\n",
           (unsigned)HAL_GPIO_ReadPin(CAM_SCCB_PORT, CAM_SCL_PIN),
           (unsigned)HAL_GPIO_ReadPin(CAM_SCCB_PORT, CAM_SDA_PIN),
           (unsigned)HAL_GPIO_ReadPin(CAM_RESET_PORT, CAM_RESET_PIN),
           (unsigned)HAL_GPIO_ReadPin(CAM_PWDN_PORT, CAM_PWDN_PIN));
    printf("  XCLK(PA8) 1 ms 内检测到 %lu 个边沿 %s\r\n",
           (unsigned long)edges, (edges > 100U) ? "<- 正常输出" : "<- 异常");
  }

  /* 先整条总线扫一遍。OV3660 应该出现在 0x3C。
     扫不到任何东西 = 模组没上电/没插好；扫到别的地址 = 挂的不是这颗片子。 */
  {
    uint8_t  addr[8];
    uint32_t n;

    n = CAM_SccbScan(addr, sizeof(addr));
    printf("  扫描 0x08~0x77：找到 %lu 个器件", (unsigned long)n);
    for (uint32_t i = 0; (i < n) && (i < sizeof(addr)); i++)
    {
      printf("%s0x%02X", (i == 0U) ? "  -> " : ", ", addr[i]);
    }
    printf("\r\n");

    if (n == 0U)
    {
      uint32_t bb_n = CAM_SccbScanBitBang(addr, sizeof(addr));
      printf("  GPIO 软件 SCCB 扫描：找到 %lu 个器件", (unsigned long)bb_n);
      for (uint32_t i = 0; (i < bb_n) && (i < sizeof(addr)); i++)
      {
        printf("%s0x%02X", (i == 0U) ? "  -> " : ", ", addr[i]);
      }
      printf("\r\n");

      if (bb_n == 0U)
      {
        static const uint8_t levels[][2] = {
          { 0U, 0U }, { 0U, 1U }, { 1U, 0U }, { 1U, 1U }
        };

        printf("  尝试 PA1/PA2 四种电平组合（排除 PWDN/RESETB 互换或极性变化）：\r\n");
        for (uint32_t k = 0; k < (sizeof(levels) / sizeof(levels[0])); k++)
        {
          CAM_SetPwdn(levels[k][0] != 0U);
          CAM_SetReset(levels[k][1] != 0U);
          HAL_Delay(60);

          bb_n = CAM_SccbScanBitBang(addr, sizeof(addr));
          printf("    PA1=%u PA2=%u -> %lu 个 ACK",
                 (unsigned)levels[k][0], (unsigned)levels[k][1],
                 (unsigned long)bb_n);
          for (uint32_t i = 0; (i < bb_n) && (i < sizeof(addr)); i++)
          {
            printf("%s0x%02X", (i == 0U) ? "  " : ", ", addr[i]);
          }
          printf("\r\n");

          if (bb_n > 0U)
          {
            n = bb_n;
            break;
          }
        }

        if (n == 0U)
        {
          CAM_SetPwdn(false);
          CAM_SetReset(true);
          HAL_Delay(50);
        }
      }
    }
  }

  ret = ov3660_read_id(&pid);
  if (ret != OV3660_OK)
  {
    fail("SCCB 读不到任何响应（I2C 无 ACK）",
         "底板 2.8V、I2C 上拉、XCLK、PWDN/RESETB 时序在前面都验过了，\r\n"
         "总线扫描也一个器件都没找到，所以问题基本锁定在这三条：\r\n"
         "\r\n"
         "  1. ★ DVDD 1.5V 没到模组 ★  量 FPC 第10脚对地，应该是 1.5V。\r\n"
         "     H25 跳线（1V5_ORI -> 1V5）不插的话，U8 输出量着正常，\r\n"
         "     但 FPC 第10脚是 0V，数字核心不上电，SCCB 永远不应答。\r\n"
         "     同理量 FPC 第11脚应为 2.8V、第4脚应为 2.8V（经 L1 磁珠）。\r\n"
         "\r\n"
         "  2. FPC 排线插反了。0.5mm 座子分上接触/下接触两种，\r\n"
         "     金手指朝向插错的话整排引脚会镜像，电源会怼到数据脚上。\r\n"
         "     对着能正常工作的那块 ESP32-S3 板，看排线是金面朝上还是朝下。\r\n"
         "\r\n"
         "  3. 排线没插到底 / 翻盖没压紧。\r\n"
         "\r\n"
         "如果上面阶段 0 的「模组在位检测」显示 11 根线全悬空，那就是 1 或 2/3。");
  }

  printf("  芯片 ID = 0x%04X", pid);
  if (pid == OV3660_CHIP_PID)
  {
    printf("   <- 正确\r\n");
  }
  else
  {
    printf("   <- 期望 0x%04X\r\n", OV3660_CHIP_PID);
    fail("SCCB 能通信但 ID 不对",
         "I2C 是通的（有 ACK），但读回的值不对。可能是：\r\n"
         "  1. 挂的不是 OV3660（0x2640=OV2640, 0x5640=OV5640, 0x7670=OV7670）\r\n"
         "  2. 供电电压不对导致寄存器读值异常 —— 重点量 DVDD(FPC 第10脚) 是不是 1.5V\r\n"
         "  3. SCCB 时序太快，试着把 CAM_I2C_TIMING 降到 100kHz 以下");
  }

  banner("阶段 2：SCCB 寄存器读写回环");

  /* 先读几个只读的身份寄存器打出来，方便和数据手册对 */
  if (ov3660_read_reg(0x302A, &v) == OV3660_OK)
  {
    printf("  芯片版本 (0x302A) = 0x%02X\r\n", v);
  }

  /*
   * 用输出宽度寄存器 0x3808/0x3809 做写-读回环测试：
   * 它是普通的读写寄存器，写错也没有危险，而且阶段 3 的软复位会把它冲掉。
   */
  if (ov3660_write_reg(0x3808, 0x01) != OV3660_OK ||
      ov3660_write_reg(0x3809, 0x40) != OV3660_OK)
  {
    fail("寄存器写入失败（无 ACK）",
         "SCCB 能读不能写，通常是：\r\n"
         "  1. SDA 上拉太弱（R44 阻值偏大）或走线太长，写周期的建立时间不够\r\n"
         "  2. I2C 速率太快，把 CAM_I2C_TIMING 调慢试试");
  }
  if (ov3660_read_reg(0x3808, &v) != OV3660_OK ||
      ov3660_read_reg(0x3809, &back) != OV3660_OK)
  {
    fail("寄存器回读失败", "SCCB 通信中断");
  }
  printf("  写 0x3808/09 = 0x01/0x40，读回 = 0x%02X/0x%02X %s\r\n",
         v, back, ((v == 0x01) && (back == 0x40)) ? "<- 正确" : "<- 不一致！");

  if ((v != 0x01) || (back != 0x40))
  {
    fail("寄存器写进去了但读回来不一样",
         "SCCB 时序有问题，或者总线上有别的器件在抢地址。\r\n"
         "先用示波器看 SCL/SDA 波形，确认高电平是 2.8V 而不是被拉到 1.x V");
  }
}

/* ---------------------------------------------------- 阶段 3：载入配置 ---- */

static void stage_configure(void)
{
  banner("阶段 3：软复位 + 载入寄存器表");

  if (ov3660_reset() != OV3660_OK)
  {
    fail("载入默认寄存器表失败", "刷表中途 SCCB 断了，多半是接触不良或电源不稳");
  }
  printf("  默认表已载入\r\n");

  if (ov3660_set_pixformat(OV3660_PIXFORMAT_RGB565) != OV3660_OK)
  {
    fail("设置 RGB565 失败", "SCCB 通信中断");
  }
  printf("  输出格式 = RGB565\r\n");

  if (ov3660_set_framesize(CAM_FRAMESIZE) != OV3660_OK)
  {
    fail("设置分辨率失败", "SCCB 通信中断");
  }
  ov3660_get_resolution(CAM_FRAMESIZE, &s_w, &s_h);
  s_frame_bytes = (uint32_t)s_w * s_h * CAM_BYTES_PER_PIXEL;

  printf("  分辨率     = %u x %u\r\n", s_w, s_h);
  printf("  单帧字节数 = %lu\r\n", (unsigned long)s_frame_bytes);
  printf("  XCLK       = %lu Hz\r\n", (unsigned long)CAM_XCLK_HZ);
  printf("  PCLK(算出) = %lu Hz\r\n", (unsigned long)ov3660_get_pclk_hz());
  printf("  单帧搬运耗时 = %lu ms（PCLK 每拍 1 字节；实际帧率还受行/场消隐限制，会更低）\r\n",
         (unsigned long)(s_frame_bytes / (ov3660_get_pclk_hz() / 1000U)));

  /* 让传感器把新配置跑稳。自动曝光/自动白平衡要十几帧才收敛，
     等太短抓出来的图会过曝、偏色，别以为是接线问题。QVGA 约 10fps，给 1.5 秒。 */
  printf("  等待 AEC/AWB 收敛 (1.5s)...\r\n");
  HAL_Delay(1500);
}

/* ---------------------------------------------------- 阶段 4：信号探测 ---- */

static bool stage_probe(void)
{
  cam_probe_t p;
  uint32_t vh_pct, hh_pct;
  bool vsync_idle_high;

  banner("阶段 4：同步信号探测（GPIO 直接采样 200ms）");

  CAM_ProbeSignals(200, &p);

  if (p.sample_count == 0U)
  {
    fail("采样失败", "SysTick 没跑起来？");
  }

  vh_pct = p.vsync_high * 100U / p.sample_count;
  hh_pct = p.href_high  * 100U / p.sample_count;

  printf("  采样次数     : %lu\r\n", (unsigned long)p.sample_count);
  printf("  PCLK  翻转   : %-8lu  高电平占比 %lu%%\r\n",
         (unsigned long)p.pclk_edges, (unsigned long)(p.pclk_high * 100U / p.sample_count));
  printf("  HREF  翻转   : %-8lu  高电平占比 %lu%%\r\n",
         (unsigned long)p.href_edges, (unsigned long)hh_pct);
  printf("  VSYNC 翻转   : %-8lu  高电平占比 %lu%%\r\n",
         (unsigned long)p.vsync_edges, (unsigned long)vh_pct);
  printf("  （PCLK 是 8MHz，GPIO 轮询远远采不过来，翻转数不为 0 就说明它在动）\r\n");

  if (p.pclk_edges == 0U)
  {
    fail("PCLK 完全没有跳变",
         "传感器没在输出像素时钟。检查：\r\n"
         "  1. PA6(PCLK) 到 FPC 第17脚的连线\r\n"
         "  2. 阶段 3 的寄存器是不是真写进去了（PLL 没配 PCLK 不会动）\r\n"
         "  3. DOVDD(FPC 第11脚) 是不是 2.8V —— 数字 IO 没电就没输出");
  }
  if (p.vsync_edges == 0U)
  {
    fail("VSYNC 没有跳变",
         "PCLK 在动但没有帧同步。检查 PB7 到 FPC 第7脚的连线；\r\n"
         "另外 PB7 在 Nucleo 上和板载蓝色 LD2 共用，建议拆掉 R50 释放这个脚");
  }
  if (p.href_edges == 0U)
  {
    printf("  [警告] HREF 没有跳变，检查 PD8 到 FPC 第9脚的连线\r\n");
  }

  /* VSYNC 在一帧里绝大部分时间处于「数据有效」的那个电平，
     少数时间处于消隐（无效）电平。DCMI 的 VSPolarity 要填的正是「无效」电平。 */
  vsync_idle_high = (vh_pct < 50U);
  printf("  推断：VSYNC 无效电平 = %s -> VSPolarity 先试 %s\r\n",
         vsync_idle_high ? "高" : "低", vsync_idle_high ? "HIGH" : "LOW");

  return vsync_idle_high;
}

/* ---------------------------------------------------- 阶段 5：DCMI 抓帧 --- */

static void stage_capture(bool vsync_active_high_guess)
{
  HAL_StatusTypeDef st;
  bool guess = vsync_active_high_guess;

  banner("阶段 5：DCMI 抓帧");

  for (int attempt = 0; attempt < 2; attempt++)
  {
    printf("  尝试 %d：VSPolarity=%s, HSPolarity=LOW, PCK=%s\r\n",
           attempt + 1, guess ? "HIGH" : "LOW", s_pclk_rising ? "RISING" : "FALLING");

    CAM_DcmiInit(guess, false, s_pclk_rising, s_frame_bytes);
    memset(s_frame, 0xA5, s_frame_bytes);   /* 填个特征值，抓完就知道有没有被覆盖 */

    st = CAM_CaptureFrame(s_frame, s_frame_bytes, 2000);
    if (st == HAL_OK)
    {
      printf("  抓帧成功\r\n");
      return;
    }

    printf("  抓帧%s，换个极性再试\r\n", (st == HAL_TIMEOUT) ? "超时" : "出错");
    CAM_DcmiDeInit();
    guess = !guess;
  }

  fail("两种 VSYNC 极性都抓不到完整帧",
       "PCLK/VSYNC 明明在动却抓不满一帧，重点查：\r\n"
       "  1. 8 根数据线有没有虚焊 —— 少一根线，DCMI 依然会等不到足够字节\r\n"
       "  2. 分辨率和缓冲区大小对不对（当前按 QVGA RGB565 = 153600 字节算）\r\n"
       "  3. 试试 HSPolarity 改成 HIGH（改 CAM_DcmiInit 的第二个参数）\r\n"
       "  4. 试试 PCLK 改成下降沿采样（把 s_pclk_rising 改成 false）");
}

/* ---------------------------------------------------- 阶段 6：画面统计 --- */

static void stage_analyze(void)
{
  uint32_t sum = 0;
  uint16_t vmin = 0xFFFF, vmax = 0;
  uint32_t untouched = 0;
  uint32_t npix = (uint32_t)s_w * s_h;

  banner("阶段 6：画面内容统计");

  for (uint32_t i = 0; i < npix; i++)
  {
    /* ★ OV 传感器的 RGB565 是【高字节先出】★
       DCMI 按接收顺序往内存里填，所以 s_frame[2i] 是高字节、s_frame[2i+1] 是低字节。
       按小端拼会得到一张结构正确但颜色乱成彩虹的图。 */
    uint16_t px = (uint16_t)(((uint16_t)s_frame[i * 2U] << 8) | s_frame[i * 2U + 1U]);
    /* RGB565 取绿色分量当亮度近似（绿占 6 bit，最能代表亮度） */
    uint8_t g = (uint8_t)((px >> 5) & 0x3FU);

    sum += g;
    if (px < vmin) { vmin = px; }
    if (px > vmax) { vmax = px; }
    if (s_frame[i * 2U] == 0xA5U && s_frame[i * 2U + 1U] == 0xA5U) { untouched++; }
  }

  printf("  像素数        : %lu\r\n", (unsigned long)npix);
  printf("  绿色分量均值  : %lu / 63\r\n", (unsigned long)(sum / npix));
  printf("  像素最小/最大 : 0x%04X / 0x%04X\r\n", vmin, vmax);
  printf("  未被覆盖的像素: %lu (%lu%%)\r\n",
         (unsigned long)untouched, (unsigned long)(untouched * 100U / npix));

  if (untouched * 100U / npix > 5U)
  {
    printf("\r\n  [警告] 有超过 5%% 的像素还是填充值 0xA5A5，说明 DMA 没写满整帧。\r\n");
    printf("         多半是某根数据线没接好，或者行/场数量和预期不符。\r\n");
  }
  else if (vmin == vmax)
  {
    printf("\r\n  [警告] 整帧像素完全一样（0x%04X）。\r\n", vmin);
    printf("         数据线可能全部悬空或全部短地/短电源。\r\n");
    printf("         建议打开彩条测试图再抓一次（见下面提示）。\r\n");
  }
  else if ((sum / npix) < 2U)
  {
    printf("\r\n  [提示] 画面几乎全黑。先确认镜头盖拿掉了、环境有光；\r\n");
    printf("         如果还是黑，用彩条测试图区分是「传感器没出图」还是「DCMI 接错」。\r\n");
  }
  else
  {
    printf("\r\n  画面有正常的明暗变化，摄像头通路验证通过。\r\n");
  }
}

/* ---------------------------------------------------- 阶段 7：导出图像 --- */

/*
 * 串口帧格式（PC 端 tools/recv_frame.py 按这个解析）：
 *   magic   "OV36"        4 字节
 *   width                2 字节，小端
 *   height               2 字节，小端
 *   bytes_per_pixel      1 字节
 *   reserved             1 字节
 *   payload              width*height*bpp 字节，RGB565 小端
 *   checksum             4 字节，payload 所有字节的和，小端
 */
static void dump_frame(void)
{
  uint8_t  hdr[10];
  uint32_t sum = 0;
  uint8_t  ck[4];

  hdr[0] = 'O'; hdr[1] = 'V'; hdr[2] = '3'; hdr[3] = '6';
  hdr[4] = (uint8_t)(s_w & 0xFFU);
  hdr[5] = (uint8_t)(s_w >> 8);
  hdr[6] = (uint8_t)(s_h & 0xFFU);
  hdr[7] = (uint8_t)(s_h >> 8);
  hdr[8] = (uint8_t)CAM_BYTES_PER_PIXEL;
  hdr[9] = 0;

  for (uint32_t i = 0; i < s_frame_bytes; i++)
  {
    sum += s_frame[i];
  }
  ck[0] = (uint8_t)(sum & 0xFFU);
  ck[1] = (uint8_t)((sum >> 8) & 0xFFU);
  ck[2] = (uint8_t)((sum >> 16) & 0xFFU);
  ck[3] = (uint8_t)((sum >> 24) & 0xFFU);

  BSP_UartWrite(hdr, sizeof(hdr));
  BSP_UartWrite(s_frame, s_frame_bytes);
  BSP_UartWrite(ck, sizeof(ck));
}

/* ------------------------------------------------------------- main ------ */

int main(void)
{
  bool     vsync_guess;
  bool     colorbar = false;
  uint32_t last_blink;

  BSP_Init();

  /* 状态灯用本板 LED6(PD2)。本板 PG2 没有走线，Nucleo 那套 LD1/LD2 也不存在。 */
  BSP_LedInit(LD3_GPIO_PORT, LD3_PIN);

  printf("\r\n\r\n");
  printf("============================================================\r\n");
  printf("  OV3660 摄像头验证 demo    STM32U575OIY6QTR @ %lu MHz\r\n",
         (unsigned long)(HAL_RCC_GetHCLKFreq() / 1000000U));
  printf("  引脚来自网表 Netlist_Schematic2_2026-09-03-02.tel\r\n");
  printf("============================================================\r\n");

  /* 先纯电气自检，确认摄像头那边真的有电、线也接上了，再去碰 I2C */
  stage_electrical();

  CAM_BindSensorIo();
  CAM_SccbInit();
  CAM_PowerUp();

  /* 在配传感器之前先扫一遍数据线短路。0.5mm FPC 相邻脚连锡的症状很隐蔽：
     图像结构完全正常，只有颜色不对，不专门测很难想到。 */
  stage_shorts();

  stage_sccb();
  stage_configure();
  vsync_guess = stage_probe();
  stage_capture(vsync_guess);
  stage_analyze();

  banner("阶段 7：导出图像 / 彩条");
  printf("  本板没有用户按键（SW1 是复位、SW2 是 CC1310 的复位），改成串口命令：\r\n");
  printf("     d  = 重新抓一帧，并把整帧原始数据从串口吐出去\r\n");
  printf("          （PC 端先跑 python tools/recv_frame.py COMx，再敲 d）\r\n");
  printf("     c  = 切换传感器内部彩条测试图\r\n");
  printf("\r\n  LED6 闪烁 = 一切正常，程序在等命令。\r\n");
  printf("  串口 %lu 8N1（USART1：PA9=TX / PA10=RX，在 H15 第 2、4 脚）\r\n\r\n",
         (unsigned long)VCP_BAUDRATE);

  last_blink = HAL_GetTick();

  while (1)
  {
    if ((HAL_GetTick() - last_blink) >= 500U)
    {
      last_blink = HAL_GetTick();
      HAL_GPIO_TogglePin(LD3_GPIO_PORT, LD3_PIN);
    }

    /* 轮询串口命令。命令很稀疏，不用中断；直接查 RXNE 然后读 RDR */
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE))
    {
      uint8_t c = (uint8_t)(huart1.Instance->RDR & 0xFFU);

      if ((c == 'd') || (c == 'D'))
      {
        printf("\r\n[导出] 重新抓一帧...\r\n");
        if (CAM_CaptureFrame(s_frame, s_frame_bytes, 2000) == HAL_OK)
        {
          dump_frame();
          printf("\r\n[导出] 完成，共 %lu 字节\r\n", (unsigned long)s_frame_bytes);
        }
        else
        {
          printf("\r\n[导出] 抓帧超时\r\n");
        }
      }
      else if ((c == 'c') || (c == 'C'))
      {
        colorbar = !colorbar;
        (void)ov3660_set_colorbar(colorbar);
        printf("\r\n[彩条测试图 %s]\r\n", colorbar ? "已打开" : "已关闭");
      }
      else if ((c != '\r') && (c != '\n'))
      {
        printf("\r\n[命令] 不认识 '%c'，用 d=导出  c=彩条\r\n", (char)c);
      }
    }
  }
}
