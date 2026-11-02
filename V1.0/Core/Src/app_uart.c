/**
  ******************************************************************************
  * @file    app_uart.c
  * @brief   串口自测（USART1 / PA9 = TX，PA10 = RX）—— 双向验证
  *
  * 网表 Netlist_Schematic2_2026-09-03-02.tel：
  *     PA10 ; H15.4 U1.E1      -> USART1_RX (AF7) -> H15 第 4 脚
  *     PA9  ; H15.2 U1.E11     -> USART1_TX (AF7) -> H15 第 2 脚
  * 板上只有这两个脚出串口，没有别的 UART 通路。
  * bsp.c 已经把它配成 921600 8N1（MX_USART1_UART_Init / HAL_UART_MspInit）。
  *
  * ---------------------------------------------------------------------------
  * 这个 demo 干什么
  * ---------------------------------------------------------------------------
  *   1. 开机打 banner + 计数器
  *   2. 【逐字节原样回显】—— 在串口助手里敲什么，它就回什么
  *   3. 每收到一个换行，打一行 [RX] 摘要（文本 + 十六进制 + 字节数）
  *   4. 每 3 秒打一行 [tick] 心跳（带累计收发字节数）
  *   5. 【整行】是 "?" 就重打 banner
  *   6. 【整行】是 "T" 就打一串已知的测试图案（给串口助手校验显示用）
  *      —— 注意是"整行"，不是"某处出现这个字符"：正文里的 T/? 照常回显，
  *         不会被当命令吃掉（这个坑实测踩过，见 main() 里的注释）
  *   7. 两颗候选 user_led 用不同速率闪烁：PD2 的 LED6 快闪 200ms、
  *      PG14 的 LED1~LED4 慢闪 1000ms。串口 '1'/'2' 可以只点其中一颗，
  *      用来确认 PCB 丝印上的 "user_led" 到底标的是哪一颗
  *
  * ---------------------------------------------------------------------------
  * 怎么判读结果
  * ---------------------------------------------------------------------------
  *   - 能看到 [tick]                -> TX (PA9) 通
  *   - 敲字能被原样回显             -> RX (PA10) 通
  *   - 只看到 [tick]、敲字不回显     -> 只有 DAP 的 RX 接到了 PA9；
  *                                     它的 TX 没接到 PA10（或者和 PA9 接反了）
  *   - 回显的是乱码                 -> 波特率/时钟不对，不是接线问题
  *
  * ---------------------------------------------------------------------------
  * ★ 为什么这里要自己开 NVIC 和写 USART1_IRQHandler ★
  *   本工程之前只用过【轮询】收串口（app_stream 里直接查 RXNE 标志），
  *   从来没走过中断，所以：
  *     - HAL_UART_MspInit 里【没有】HAL_NVIC_EnableIRQ(USART1_IRQn)
  *     - stm32u5xx_it.c 里【没有】USART1_IRQHandler
  *   这里为了让接收一个字节都不丢（主循环里有 printf，会阻塞几毫秒，
  *   921600 下 1ms 就是 92 字节，轮询肯定丢），改成中断收。
  *   为了不动公共文件，NVIC 和中断向量都放在本文件里，自成一体。
  *
  * 编译：.\build.ps1 APP=uart
  ******************************************************************************
  */

#include "bsp.h"
#include <string.h>
#include <stdbool.h>

/* ------------------------------------------------------------- 接收环形缓冲 */
#define UART_RX_RING    128U

static volatile uint8_t  s_ring[UART_RX_RING];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;
static volatile uint32_t s_rx_bytes;      /* 累计收到的字节数 */
static volatile uint32_t s_rx_drop;       /* 环形缓冲满了丢掉的字节数 */
static volatile uint32_t s_rx_err;        /* ORE/FE/NE/PE 出错次数 */
static uint8_t           s_rx_byte;

/* 接收完成：塞进环形缓冲，然后立刻重新挂上接收 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == VCP_UART)
  {
    uint16_t next = (uint16_t)((s_head + 1U) % UART_RX_RING);

    if (next != s_tail)
    {
      s_ring[s_head] = s_rx_byte;
      s_head = next;
      s_rx_bytes++;
    }
    else
    {
      s_rx_drop++;
    }

    (void)HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1);
  }
}

/* 中断向量。startup 里的同名弱符号会被这个覆盖掉 */
void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}

/*
 * ★ 出错回调：不写这个，RX 会"用着用着就死了" ★
 *
 * 921600 下主循环里随便一次 printf 都要阻塞几百微秒到几毫秒，
 * 这段时间来的字节很容易把只有 1 级缓冲的 RX 顶溢出（ORE）。
 * HAL 一旦看到 ORE/FE/NE/PE，会走错误分支并把接收状态机停掉 ——
 * 之后再也不会有 RxCplt 回调，表现为"前面收得好好的，突然一个字都收不到了"。
 *
 * 所以这里必须：清标志 + 把状态机掰回 READY + 重新挂上接收。
 *   ORE/FE/NE/PE 的标准清除序列就是「读 ISR，再读 RDR」。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == VCP_UART)
  {
    volatile uint32_t tmp;
    tmp = huart->Instance->ISR;
    tmp = huart->Instance->RDR;
    (void)tmp;

    s_rx_err++;

    huart->RxState = HAL_UART_STATE_READY;
    (void)HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1);
  }
}

/* ------------------------------------------------------------- 工具 -------- */

static void uart_put(const char *s)
{
  BSP_UartWrite((const uint8_t *)s, (uint32_t)strlen(s));
}

static void banner(void)
{
  printf("\r\n\r\n============================================================\r\n");
  printf("  串口自测 (USART1)    STM32U575OIY6QTR   自研板 Schematic2\r\n");
  printf("============================================================\r\n");
  printf("  PA9  (H15.2) = USART1_TX   -> 探针的 RX\r\n");
  printf("  PA10 (H15.4) = USART1_RX   <- 探针的 TX\r\n");
  printf("  GND  (H15.5) 必须共地\r\n");
  printf("  921600 8N1\r\n\r\n");
  printf("  SYSCLK = %lu Hz\r\n", (unsigned long)HAL_RCC_GetSysClockFreq());
  printf("  HCLK   = %lu Hz\r\n", (unsigned long)HAL_RCC_GetHCLKFreq());
  printf("  UID    = %08lX-%08lX-%08lX\r\n",
         (unsigned long)HAL_GetUIDw2(), (unsigned long)HAL_GetUIDw1(),
         (unsigned long)HAL_GetUIDw0());
  printf("  累计收到 %lu 字节，丢弃 %lu 字节，出错 %lu 次\r\n",
         (unsigned long)s_rx_bytes, (unsigned long)s_rx_drop,
         (unsigned long)s_rx_err);
  printf("\r\n  命令（必须【单独一行】，打完要回车）：\r\n");
  printf("      ?  = 重打本页\r\n");
  printf("      T  = 打一串测试图案\r\n");
  printf("      L  = 两颗 LED 闪烁（默认态）\r\n");
  printf("      1  = 只点 PD2  -> LED6（单颗 0603）常亮\r\n");
  printf("      2  = 只点 PG14 -> LED1~LED4（四颗白光）常亮\r\n");
  printf("      0  = 两颗都灭\r\n");
  printf("  其它任何内容：原样回显 + 回一行 [RX] 统计（含 HEX）\r\n");
  printf("  正文里出现命令字符不会被吃掉，只有整行等于它才算命令\r\n\r\n");
}

static void test_pattern(void)
{
  uart_put("\r\n--- TEST PATTERN ---\r\n");
  uart_put("0123456789\r\n");
  uart_put("ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n");
  uart_put("abcdefghijklmnopqrstuvwxyz\r\n");
  uart_put("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\r\n");
  uart_put("--- END ---\r\n\r\n");
}

/* --------------------------------------------------------- LED 模式 --------- */
/*
 * 0 = 闪烁（默认）：PD2 快闪 200ms，PG14 慢闪 1000ms
 *                    —— 两个速率不一样，一眼就能分清哪颗是哪颗
 * 1 = 只点 PD2  (LED6)   常亮
 * 2 = 只点 PG14 (LED1~4) 常亮
 * 3 = 两颗都灭
 *
 * 灌电流接法：拉低才亮。所以"亮"= RESET，"灭"= SET。
 */
#define LEDM_BLINK   0U
#define LEDM_PD2     1U
#define LEDM_PG14    2U
#define LEDM_OFF     3U

static void led_apply(uint8_t m)
{
  switch (m)
  {
    case LEDM_PD2:
      HAL_GPIO_WritePin(GPIOD, GPIO_PIN_2,  GPIO_PIN_RESET);   /* LED6 亮 */
      HAL_GPIO_WritePin(GPIOG, GPIO_PIN_14, GPIO_PIN_SET);     /* 白光灭 */
      break;
    case LEDM_PG14:
      HAL_GPIO_WritePin(GPIOD, GPIO_PIN_2,  GPIO_PIN_SET);
      HAL_GPIO_WritePin(GPIOG, GPIO_PIN_14, GPIO_PIN_RESET);
      break;
    case LEDM_OFF:
      HAL_GPIO_WritePin(GPIOD, GPIO_PIN_2,  GPIO_PIN_SET);
      HAL_GPIO_WritePin(GPIOG, GPIO_PIN_14, GPIO_PIN_SET);
      break;
    default:                       /* LEDM_BLINK：交给主循环去翻 */
      break;
  }
}

/* --------------------------------------------------------------- main ------ */

int main(void)
{
  static char    line[80];
  uint8_t        line_len = 0;
  uint32_t       t_led    = 0;          /* PG14 慢闪用 */
  uint32_t       t_led6   = 0;          /* PD2  快闪用 */
  uint32_t       t_tick   = 0;
  uint32_t       tx_bytes = 0;
  uint8_t        ledmode  = LEDM_BLINK;

  BSP_Init();

  /*
   * ---- 本板两颗可以当 user_led 的灯，一起驱动 ----
   *
   * PCB 顶层丝印上印着 "user_led"（pcb_geber\v2-01\...\Gerber_TopSilkscreenLayer.GTO
   * 里的 G04 //text: user_led，位置 X=8.09 Y=-2.90），但整块 gerber 只导出了元件【值】
   * 没有位号，所以没法从文件上确定它标的是哪一颗。本板能当 user_led 的只有两颗：
   *
   *     PD2  (U1.A5) -> R85 -> LED6        (0603 单颗)
   *     PG14 (U1.A9) -> LED1~LED4          (四颗白光并联)
   *
   * 两颗都是【灌电流】接法：引脚拉低才亮。
   * 这里两颗一起闪 —— 看板子上哪一颗在动，那就是丝印说的 user_led。
   * 串口发 'L' 可以让它们常亮（比闪烁更容易肉眼确认），再发一次回到闪烁。
   */
  {
    GPIO_InitTypeDef gi = {0};
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_2,  GPIO_PIN_SET);   /* 先写"灭"（高电平） */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_14, GPIO_PIN_SET);

    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;

    gi.Pin = GPIO_PIN_2;                                   /* PD2  */
    HAL_GPIO_Init(GPIOD, &gi);
    gi.Pin = GPIO_PIN_14;                                  /* PG14 */
    HAL_GPIO_Init(GPIOG, &gi);
  }

  /* 串口接收改成中断驱动，先自己把 NVIC 打开 */
  HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  (void)HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1);

  banner();

  while (1)
  {
    uint32_t now = HAL_GetTick();

    /* ---- 收：把环形缓冲里的字节取出来 ---- */
    while (s_tail != s_head)
    {
      uint8_t b = s_ring[s_tail];
      s_tail = (uint16_t)((s_tail + 1U) % UART_RX_RING);

      /* 原样回显（TX 方向） */
      BSP_UartWrite(&b, 1);
      tx_bytes++;

      /* 攒行 */
      if ((b == '\r') || (b == '\n'))
      {
        if (line_len > 0U)
        {
          line[line_len] = '\0';

          /*
           * ★ 命令只在【整行】就是它的时候才认 ★
           *   一开始写成"收到 '?'/'T' 就立刻执行"，结果正文里只要出现一个大写 T
           *   就会被吃掉（实测发 "0123...TUVWXYZ..." 时直接在中间插了一段测试图案，
           *   还把已经攒好的行清空了）。串口助手里打一句 "TEST" 就中招。
           */
          if ((line_len == 1U) && (line[0] == '?'))
          {
            banner();
          }
          else if ((line_len == 1U) && (line[0] == 'T'))
          {
            test_pattern();
          }
          else if ((line_len == 1U) && ((line[0] == 'L') || (line[0] == 'l')))
          {
            ledmode = LEDM_BLINK;
            led_apply(ledmode);
            printf("\r\n[LED] 回到闪烁：PD2(LED6) 快闪 200ms，PG14(LED1~4) 慢闪 1000ms\r\n");
          }
          else if ((line_len == 1U) && (line[0] == '1'))
          {
            ledmode = LEDM_PD2;
            led_apply(ledmode);
            printf("\r\n[LED] 只点 PD2 -> LED6（单颗 0603），常亮\r\n");
          }
          else if ((line_len == 1U) && (line[0] == '2'))
          {
            ledmode = LEDM_PG14;
            led_apply(ledmode);
            printf("\r\n[LED] 只点 PG14 -> LED1~LED4（四颗白光），常亮\r\n");
          }
          else if ((line_len == 1U) && (line[0] == '0'))
          {
            ledmode = LEDM_OFF;
            led_apply(ledmode);
            printf("\r\n[LED] 两颗都灭\r\n");
          }
          else
          {
            uint8_t i;
            printf("\r\n[RX] %u 字节，累计 %lu | 文本: %s\r\n",
                   (unsigned)line_len, (unsigned long)s_rx_bytes, line);
            printf("     HEX : ");
            for (i = 0; i < line_len; i++)
            {
              printf("%02X ", (unsigned)(uint8_t)line[i]);
            }
            printf("\r\n");
          }
          line_len = 0;
        }
      }
      else if (line_len < (sizeof(line) - 1U))
      {
        line[line_len++] = (char)b;
      }
      else
      {
        line[line_len] = '\0';
        printf("\r\n[RX] 行超过 %u 字节，已截断\r\n", (unsigned)(sizeof(line) - 1U));
        line_len = 0;
      }
    }

    /* ---- 闪烁模式：两颗用不同速率，方便分清哪颗是哪颗 ---- */
    if (ledmode == LEDM_BLINK)
    {
      if ((uint32_t)(now - t_led6) >= 200U)      /* PD2  -> LED6     快闪 */
      {
        t_led6 = now;
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_2);
      }
      if ((uint32_t)(now - t_led) >= 1000U)      /* PG14 -> LED1~4   慢闪 */
      {
        t_led = now;
        HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_14);
      }
    }

    /* ---- 每 3 秒一行心跳，证明 TX 一直在 ---- */
    if ((uint32_t)(now - t_tick) >= 3000U)
    {
      t_tick = now;
      printf("[tick] %lu ms  rx=%lu 丢弃=%lu 出错=%lu  tx≈%lu\r\n",
             (unsigned long)now, (unsigned long)s_rx_bytes,
             (unsigned long)s_rx_drop, (unsigned long)s_rx_err,
             (unsigned long)tx_bytes);
    }
  }
}
