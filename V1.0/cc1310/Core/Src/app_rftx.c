/**
  ******************************************************************************
  * @file    app_rftx.c
  * @brief   CC1310 射频发射自检：每秒发一包，串口打结果
  *
  * 这是射频链路的最小验证 —— 先确认"能不能发出去"，
  * 再谈把 SPI 收到的图像数据接进来。
  *
  * 串口：DIO3(TX)/DIO2(RX) -> XDS110 Application UART，本机 COM18，115200 8N1
  *
  * 怎么确认真的发出去了（三选一）：
  *   1. 另一块 CC1310 跑接收（最直接，但要第二块板）
  *   2. 频谱仪 / SDR（RTL-SDR 就够）看中心频点有没有周期性的包
  *   3. 只看串口：RFP_Init 返回 OK 说明【合成器已经锁在目标频率上】，
  *      CMD_PROP_TX 返回 PROP_DONE_OK 说明 RF core 认为包已发完。
  *      这不能证明天线真的辐射了，但能证明射频链路的数字部分全通。
  ******************************************************************************
  */

#include <stdint.h>
#include <stdbool.h>

#include <driverlib/prcm.h>
#include <driverlib/ioc.h>
#include <driverlib/uart.h>
#include <driverlib/cpu.h>
#include <inc/hw_memmap.h>

#include "board.h"
#include "rf_prop.h"
#include "rf_prop_band.h"

#define CPU_HZ          48000000U
#define DELAY_MS(ms)    CPUdelay(((CPU_HZ / 1000U) / 3U) * (ms))

static void periph_power_on(void)
{
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON) { }
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
    PRCMLoadSet();
    while (!PRCMLoadGet()) { }
}

static void uart_init(void)
{
    IOCPinTypeUart(UART0_BASE, BOARD_UART_RX, BOARD_UART_TX,
                   IOID_UNUSED, IOID_UNUSED);
    UARTConfigSetExpClk(UART0_BASE, CPU_HZ, 115200U,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                        UART_CONFIG_PAR_NONE);
    UARTEnable(UART0_BASE);
}

static void uart_puts(const char *s)
{
    while (*s != '\0')
    {
        if (*s == '\n') { UARTCharPut(UART0_BASE, '\r'); }
        UARTCharPut(UART0_BASE, (uint8_t)*s++);
    }
}

static void uart_u32(const char *label, uint32_t v)
{
    char buf[12];
    int  i = 0;
    uart_puts(label);
    if (v == 0U) { uart_puts("0"); }
    else
    {
        while (v > 0U) { buf[i++] = (char)('0' + (v % 10U)); v /= 10U; }
        while (i > 0)  { UARTCharPut(UART0_BASE, (uint8_t)buf[--i]); }
    }
    uart_puts("\n");
}

static void uart_hex16(const char *label, uint16_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    uart_puts(label);
    uart_puts("0x");
    for (i = 12; i >= 0; i -= 4)
    {
        UARTCharPut(UART0_BASE, (uint8_t)hx[(v >> i) & 0xFU]);
    }
    uart_puts("\n");
}

int main(void)
{
    rfp_status_t st;
    uint32_t seq = 0;
    uint8_t  pkt[32];
    uint32_t ok = 0;
    uint32_t fail = 0;

    periph_power_on();
    uart_init();

    uart_puts("\n============================================\n");
    uart_puts("  CC1310 射频发射自检 (prop mode, 2-GFSK)\n");
    uart_u32("  中心频率 (MHz) : ", RFP_CENTER_FREQ);
    uart_u32("  loDivider      : ", RFP_LO_DIVIDER);
    uart_puts("============================================\n");

    uart_puts("初始化射频…\n");
    st = RFP_Init();
    if (st != RFP_OK)
    {
        uart_puts("[失败] ");
        uart_puts(RFP_StatusStr(st));
        uart_puts("\n");
        uart_hex16("  RF core 状态字 : ", RFP_LastCmdStatus());
        uart_puts("\n对照表：\n");
        uart_puts("  0x0400 DONE_OK        命令正常结束\n");
        uart_puts("  0x0801 ERROR_PAST_START  起始时间已过\n");
        uart_puts("  0x0802 ERROR_START_TRIG  触发条件非法\n");
        uart_puts("  0x0807 ERROR_NO_SETUP    没先跑 SETUP 命令\n");
        uart_puts("  0x0808 ERROR_NO_FS       没先锁频\n");
        uart_puts("  0x0809 ERROR_SYNTH_PROG  合成器编程失败\n");
        while (1) { }
    }

    uart_puts("[OK] 合成器已锁定，开始每秒发一包\n\n");

    while (1)
    {
        uint32_t i;

        /* 包内容：4 字节序号 + 递增填充，接收端好校验 */
        pkt[0] = (uint8_t)(seq >> 24);
        pkt[1] = (uint8_t)(seq >> 16);
        pkt[2] = (uint8_t)(seq >> 8);
        pkt[3] = (uint8_t)seq;
        for (i = 4; i < sizeof(pkt); i++)
        {
            pkt[i] = (uint8_t)i;
        }

        st = RFP_Send(pkt, (uint8_t)sizeof(pkt));
        if (st == RFP_OK)
        {
            ok++;
        }
        else
        {
            fail++;
        }

        if ((seq % 10U) == 0U)
        {
            uart_u32("已发 ", seq);
            uart_u32("  成功 ", ok);
            uart_u32("  失败 ", fail);
            if (fail > 0U)
            {
                uart_hex16("  最近状态字 ", RFP_LastCmdStatus());
            }
        }

        seq++;
        DELAY_MS(1000);
    }
}
