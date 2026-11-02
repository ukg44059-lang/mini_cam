/**
  ******************************************************************************
  * @file    app_spirx.c
  * @brief   CC1310 当 SPI 从机，接收 STM32 发来的数据并校验（链路第一阶段）
  *
  * 硬件连接（底板 H7 排针 -> 杜邦线 -> LaunchPad）：
  *
  *   STM32U575        底板 H7    LaunchPad CC1310
  *   ---------------------------------------------------------
  *   PE13 SPI1_SCK    H7.1  -->  DIO9   SSI0_CLK
  *   PE14 SPI1_MISO   H7.2  <--  DIO8   SSI0_TX
  *   PE15 SPI1_MOSI   H7.3  -->  DIO6   SSI0_RX
  *   PE12 SPI1_NSS    H7.4  -->  DIO7   SSI0_FSS
  *   GND              H7.5/6 --- GND    ★ 地一定要接，不接现象千奇百怪 ★
  *
  * ⚠ DIO6/DIO7 在 LaunchPad 上是红/绿 LED
  *   这是用户选定的映射。后果：
  *     - CS(DIO7) 空闲为高 -> 绿灯常亮
  *     - MOSI(DIO6) 翻转   -> 红灯跟着乱闪
  *   所以本 demo 的状态指示【不能用 LED】，全部走 UART。
  *   LED 那条支路（LED + 限流电阻到地）对 SPI 来说是个轻负载，
  *   低速（≤4MHz）下能正常工作；如果以后提速出现误码，第一个要怀疑它。
  *
  * 串口日志：DIO3(TX) / DIO2(RX) -> 板载 XDS110 的 Application UART
  *           本机是 COM18，115200 8N1
  *
  * ---------------------------------------------------------------------------
  * 测试协议（和 STM32 侧 app_spitx.c 对应）
  * ---------------------------------------------------------------------------
  * STM32 每轮拉低 CS，连发 256 字节：0x00,0x01,...,0xFF，然后抬高 CS。
  *
  *   正向(MOSI)：CC1310 校验收到的是不是递增序列  -> 证明 MOSI/SCK/CS 通
  *   反向(MISO)：CC1310 预先往 TX FIFO 灌 'C','C','1','3' 循环，
  *               STM32 读回来找这个签名                -> 证明 MISO 通
  *
  * ★ SPI 从机的一个坑 ★
  * 从机不产生时钟，主机一开始打时钟就要有数据可发。所以 TX 必须【提前】
  * 灌进 FIFO，等收到再回就晚了一拍。这也是为什么从机的回读天然比发送
  * 落后一个字节，STM32 那边校验时要考虑这个错位。
  ******************************************************************************
  */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <driverlib/prcm.h>
#include <driverlib/ioc.h>
#include <driverlib/gpio.h>
#include <driverlib/ssi.h>
#include <driverlib/uart.h>
#include <driverlib/cpu.h>
#include <inc/hw_memmap.h>

#include "board.h"

/* ---- SPI 从机引脚（用户指定的映射，见文件头） ---- */
#define SPI_RX_DIO      IOID_6      /* MOSI，STM32 -> CC1310。板上也是红LED */
#define SPI_FSS_DIO     IOID_7      /* CS，                    板上也是绿LED */
#define SPI_TX_DIO      IOID_8      /* MISO，CC1310 -> STM32 */
#define SPI_CLK_DIO     IOID_9      /* SCK */

#define TEST_LEN        256U        /* STM32 每轮发多少字节 */

static const char SIGNATURE[4] = { 'C', 'C', '1', '3' };

/* ------------------------------------------------------------ 基础设施 ---- */

static void periph_power_on(void)
{
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON) { }

    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMPeripheralRunEnable(PRCM_PERIPH_SSI0);
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);

    /* ★ 两段式时钟使能：上面那几句写完必须再触发一次加载才生效 ★ */
    PRCMLoadSet();
    while (!PRCMLoadGet()) { }
}

/* ---- UART：只用来打日志，115200 8N1 ---- */
static void uart_init(void)
{
    IOCPinTypeUart(UART0_BASE, BOARD_UART_RX, BOARD_UART_TX,
                   IOID_UNUSED, IOID_UNUSED);

    /* 复位后 CPU/外设跑在 48MHz */
    UARTConfigSetExpClk(UART0_BASE, 48000000U, 115200U,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                        UART_CONFIG_PAR_NONE);
    UARTEnable(UART0_BASE);
}

static void uart_puts(const char *s)
{
    while (*s != '\0')
    {
        if (*s == '\n')
        {
            UARTCharPut(UART0_BASE, '\r');
        }
        UARTCharPut(UART0_BASE, (uint8_t)*s++);
    }
}

/* 避免拉进 newlib 的 printf（几十 KB flash，20KB RAM 的片子吃不消） */
static void uart_u32(const char *label, uint32_t v)
{
    char buf[12];
    int  i = 0;

    uart_puts(label);
    if (v == 0U)
    {
        uart_puts("0");
    }
    else
    {
        while (v > 0U)
        {
            buf[i++] = (char)('0' + (v % 10U));
            v /= 10U;
        }
        while (i > 0)
        {
            UARTCharPut(UART0_BASE, (uint8_t)buf[--i]);
        }
    }
    uart_puts("\n");
}

/* ---- SSI0 从机 ---- */
static void spi_slave_init(void)
{
    IOCPinTypeSsiSlave(SSI0_BASE, SPI_RX_DIO, SPI_TX_DIO,
                       SPI_FSS_DIO, SPI_CLK_DIO);

    /*
     * 从机模式下第二个参数（SSI 时钟）只用于内部分频计算，
     * 实际速率由主机的 SCK 决定，填 48MHz 即可。
     * Motorola 模式 0（CPOL=0, CPHA=0），8 位 —— STM32 那边必须配成一样。
     */
    SSIConfigSetExpClk(SSI0_BASE, 48000000U, SSI_FRF_MOTO_MODE_0,
                       SSI_MODE_SLAVE, 4000000U, 8);
    SSIEnable(SSI0_BASE);
}

/**
  * @brief 把签名灌满 TX FIFO
  * @note  从机不产时钟，主机一开始打时钟就得有数据可发，
  *        所以必须【提前】填，不能等收到再回。
  */
static void spi_preload_tx(uint32_t *sig_idx)
{
    uint32_t dummy;

    /* 先把可能残留的旧数据读干净，否则会和这一轮的数据错位 */
    while (SSIDataGetNonBlocking(SSI0_BASE, &dummy) != 0) { }

    while (SSIDataPutNonBlocking(SSI0_BASE,
                                 (uint8_t)SIGNATURE[*sig_idx & 3U]) != 0)
    {
        (*sig_idx)++;
    }
}

/* --------------------------------------------------------------- main ---- */

int main(void)
{
    uint32_t round   = 0;
    uint32_t sig_idx = 0;

    periph_power_on();
    uart_init();
    spi_slave_init();

    uart_puts("\n============================================\n");
    uart_puts("  CC1310 SPI 从机 (SSI0)  等 STM32 发数据\n");
    uart_puts("  MOSI=DIO6  CS=DIO7  MISO=DIO8  SCK=DIO9\n");
    uart_puts("  模式0 (CPOL=0,CPHA=0), 8bit, MSB first\n");
    uart_puts("  注意 DIO6/DIO7 板上是红/绿LED，会跟着闪\n");
    uart_puts("============================================\n");

    spi_preload_tx(&sig_idx);

    while (1)
    {
        uint32_t got  = 0;
        uint32_t errs = 0;
        uint32_t first = 0xFFFFFFFFU;
        uint32_t last  = 0;
        uint32_t idle  = 0;
        uint32_t d;

        /* 收满一轮 TEST_LEN 字节，或者线上静默一段时间就认为这轮结束 */
        while (got < TEST_LEN)
        {
            if (SSIDataGetNonBlocking(SSI0_BASE, &d) != 0)
            {
                if (got == 0U)
                {
                    first = d;
                }
                /* 期望是 0x00,0x01,... 递增 */
                if ((uint8_t)d != (uint8_t)got)
                {
                    errs++;
                }
                last = d;
                got++;
                idle = 0;

                /* 收一个补一个，保证 TX FIFO 不空 */
                (void)SSIDataPutNonBlocking(SSI0_BASE,
                                            (uint8_t)SIGNATURE[sig_idx++ & 3U]);
            }
            else
            {
                idle++;
                if ((got > 0U) && (idle > 2000000U))
                {
                    break;              /* 收了一半就没了，也报出来 */
                }
                if ((got == 0U) && (idle > 40000000U))
                {
                    break;              /* 一直没数据，打个心跳 */
                }
            }
        }

        if (got == 0U)
        {
            uart_puts("[等待] 还没收到任何 SPI 数据…\n");
            uart_puts("       查：GND 接了吗？CS/SCK/MOSI 有没有接反？\n");
            continue;
        }

        round++;
        uart_puts("\n---- 第 ");
        uart_u32("", round);
        uart_u32("  收到字节数 : ", got);
        uart_u32("  首字节     : ", first);
        uart_u32("  末字节     : ", last);
        uart_u32("  校验错     : ", errs);

        if ((got == TEST_LEN) && (errs == 0U) &&
            (first == 0U) && (last == (TEST_LEN - 1U)))
        {
            uart_puts("  >>> 正向链路 OK：256 字节递增序列完全正确\n");
        }
        else
        {
            uart_puts("  >>> 有问题。first 应为 0、last 应为 255、错误应为 0\n");
        }

        spi_preload_tx(&sig_idx);
    }
}
