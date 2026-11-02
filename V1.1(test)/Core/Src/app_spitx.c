/**
  ******************************************************************************
  * @file    app_spitx.c
  * @brief   STM32U575 当 SPI 主机，往 CC1310 发数据（链路第一阶段）
  *
  * 硬件连接（底板 H7 排针 -> 杜邦线 -> LaunchPad-CC1310）：
  *
  *   STM32U575        底板 H7    LaunchPad CC1310
  *   ---------------------------------------------------------
  *   PE13 SPI1_SCK    H7.1  -->  DIO9   SSI0_CLK
  *   PE14 SPI1_MISO   H7.2  <--  DIO8   SSI0_TX
  *   PE15 SPI1_MOSI   H7.3  -->  DIO6   SSI0_RX
  *   PE12 SPI1_NSS    H7.4  -->  DIO7   SSI0_FSS
  *   GND              H7.5/6 --- GND    ★ 地一定要接 ★
  *
  * 引脚全部是 AF5，已对照 DS13737 Rev10 的 AF 表逐条核实：
  *   PE12=SPI1_NSS  PE13=SPI1_SCK  PE14=SPI1_MISO  PE15=SPI1_MOSI
  * 和摄像头（只占 PE4）、USB（PA11/12）、日志串口（PA9/10）都不冲突。
  *
  * ---------------------------------------------------------------------------
  * 测试内容（和 CC1310 侧 app_spirx.c 对应）
  * ---------------------------------------------------------------------------
  *   正向：每轮发 256 字节 0x00..0xFF，CC1310 校验递增序列 -> 验 MOSI/SCK/CS
  *   反向：同时读回 MISO，找 CC1310 预置的 'C','C','1','3' 签名 -> 验 MISO
  *
  * ★ 从机回读天然错位 ★
  * SPI 从机不产生时钟，主机第一个字节的时钟打出去时，从机还没来得及
  * "看到"这个字节就得开始往外移位。所以从机的响应总是比它收到的晚一拍，
  * 校验时不能按字节一一对应，只能在读回的整段里【找签名子串】。
  *
  * ---------------------------------------------------------------------------
  * 速率
  * ---------------------------------------------------------------------------
  * 默认 SCK 用 PCLK/64。SPI1 挂在 APB2（160MHz），也就是 2.5 MHz。
  * 第一次连线故意跑慢：DIO6/DIO7 在 LaunchPad 上还挂着 LED，杜邦线也没有
  * 阻抗控制，先确认逻辑通，再谈提速。
  * 通了之后把 BaudRatePrescaler 往小调（/32=5MHz、/16=10MHz…）逐级试。
  ******************************************************************************
  */

#include "bsp.h"
#include <string.h>

/* ---------------- 引脚 ---------------- */
#define SPI_GPIO_PORT       GPIOE
#define SPI_NSS_PIN         GPIO_PIN_12
#define SPI_SCK_PIN         GPIO_PIN_13
#define SPI_MISO_PIN        GPIO_PIN_14
#define SPI_MOSI_PIN        GPIO_PIN_15
#define SPI_GPIO_AF         GPIO_AF5_SPI1

#define TEST_LEN            256U

static SPI_HandleTypeDef hspi1;

static uint8_t s_tx[TEST_LEN];
static uint8_t s_rx[TEST_LEN];

static void spi_master_init(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    gi.Pin       = SPI_NSS_PIN | SPI_SCK_PIN | SPI_MISO_PIN | SPI_MOSI_PIN;
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = SPI_GPIO_AF;
    HAL_GPIO_Init(SPI_GPIO_PORT, &gi);

    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    /* 必须和 CC1310 的 SSI_FRF_MOTO_MODE_0 一致：CPOL=0, CPHA=0 */
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    /*
     * 用硬件 NSS：让 SPI 外设自己在整段传输期间把 CS 拉低。
     * NSSPMode 必须【关掉】—— 打开的话每个字节之间 CS 都会抬一次，
     * CC1310 那边会把一次传输当成 256 次，收到的数据全是错位的。
     */
    hspi1.Init.NSS               = SPI_NSS_HARD_OUTPUT;
    hspi1.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;   /* 160/64 = 2.5MHz */
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;

    if (HAL_SPI_Init(&hspi1) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
  * @brief 在读回的数据里找 CC1310 预置的签名
  * @retval 找到的位置，找不到返回 -1
  */
static int find_signature(const uint8_t *buf, uint32_t len)
{
    static const uint8_t sig[4] = { 'C', 'C', '1', '3' };
    uint32_t i;

    if (len < 4U)
    {
        return -1;
    }
    for (i = 0; i <= (len - 4U); i++)
    {
        if (memcmp(&buf[i], sig, 4) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

int main(void)
{
    uint32_t round = 0;
    uint32_t i;

    BSP_Init();

    printf("\r\n============================================================\r\n");
    printf("  STM32U575 SPI1 主机 -> CC1310 SSI0 从机\r\n");
    printf("  SCK=PE13  MISO=PE14  MOSI=PE15  CS=PE12   全部 AF5\r\n");
    printf("  模式0 (CPOL=0,CPHA=0), 8bit, MSB first, SCK=2.5MHz\r\n");
    printf("  每轮发 256 字节 0x00..0xFF，同时读回 MISO 找签名\r\n");
    printf("============================================================\r\n");

    spi_master_init();

    for (i = 0; i < TEST_LEN; i++)
    {
        s_tx[i] = (uint8_t)i;
    }

    BSP_LedInit(LD3_GPIO_PORT, LD3_PIN);

    while (1)
    {
        HAL_StatusTypeDef st;
        int sigpos;

        memset(s_rx, 0, sizeof(s_rx));

        st = HAL_SPI_TransmitReceive(&hspi1, s_tx, s_rx, TEST_LEN, 1000);
        round++;

        printf("\r\n---- 第 %lu 轮 ----\r\n", (unsigned long)round);
        if (st != HAL_OK)
        {
            printf("  HAL_SPI_TransmitReceive 失败: %d\r\n", (int)st);
            HAL_Delay(1000);
            continue;
        }

        /* 读回的前 16 字节，用来肉眼看 MISO 上到底是什么 */
        printf("  MISO 前16字节:");
        for (i = 0; i < 16U; i++)
        {
            printf(" %02X", s_rx[i]);
        }
        printf("\r\n");

        sigpos = find_signature(s_rx, TEST_LEN);
        if (sigpos >= 0)
        {
            printf("  >>> 反向链路 OK：第 %d 字节处找到签名 'CC13'\r\n", sigpos);
        }
        else
        {
            uint32_t all_ff = 0;
            uint32_t all_00 = 0;
            for (i = 0; i < TEST_LEN; i++)
            {
                if (s_rx[i] == 0xFFU) { all_ff++; }
                if (s_rx[i] == 0x00U) { all_00++; }
            }
            printf("  >>> MISO 上没找到签名。");
            if (all_ff == TEST_LEN)
            {
                printf("全是 FF —— MISO 悬空（线没接/接错脚）\r\n");
            }
            else if (all_00 == TEST_LEN)
            {
                printf("全是 00 —— MISO 被拉到地，或 CC1310 没在跑\r\n");
            }
            else
            {
                printf("有数据但对不上 —— 查 CPOL/CPHA 和位序\r\n");
            }
        }

        HAL_GPIO_TogglePin(LD3_GPIO_PORT, LD3_PIN);
        HAL_Delay(1000);
    }
}
