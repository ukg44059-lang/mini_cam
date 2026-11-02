#ifndef RF433_DEMO_IMPL_H
#define RF433_DEMO_IMPL_H

#include <stdint.h>
#include <string.h>
#include <driverlib/cpu.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/uart.h>
#include <inc/hw_memmap.h>
#include "rf_prop.h"

#if RFP_BAND != 433
#error "Build this demo with BAND=433"
#endif
#if RF433_BOARD != 1 && RF433_BOARD != 2
#error "RF433_BOARD must be 1 or 2"
#endif

extern volatile uint32_t g_rfp_debug_stage;
volatile uint32_t g_rf433_tx_count;
volatile uint32_t g_rf433_rx_count;
volatile uint32_t g_rf433_timeout_count;
volatile uint32_t g_rf433_error_count;
volatile uint32_t g_rf433_init_result;

static void demo_delay_ms(uint32_t ms)
{
    while (ms-- != 0U) CPUdelay(16000U);
}

static void demo_uart_init(void)
{
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL);
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON) { }
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) != PRCM_DOMAIN_POWER_ON) { }
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
    PRCMLoadSet();
    while (!PRCMLoadGet()) { }
    IOCPinTypeUart(UART0_BASE, RF433_UART_RX, RF433_UART_TX,
                   IOID_UNUSED, IOID_UNUSED);
    UARTDisable(UART0_BASE);
    UARTConfigSetExpClk(UART0_BASE, 48000000U, 115200U,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                        UART_CONFIG_PAR_NONE);
    UARTEnable(UART0_BASE);
}

static void demo_puts(const char *p)
{
    while (*p) UARTCharPut(UART0_BASE, (uint8_t)*p++);
}

static void demo_u32(uint32_t n)
{
    char b[10];
    uint32_t count = 0U;
    do { b[count++] = (char)('0' + n % 10U); n /= 10U; } while (n);
    while (count) UARTCharPut(UART0_BASE, (uint8_t)b[--count]);
}

static void demo_error(const char *label, rfp_status_t status)
{
    static const char hex[] = "0123456789ABCDEF";
    uint16_t cmd = RFP_LastCmdStatus();
    int shift;
    demo_puts(label);
    demo_puts(" code="); demo_u32(status);
    demo_puts(" stage="); demo_u32(g_rfp_debug_stage);
    demo_puts(" cmd=0x");
    for (shift = 12; shift >= 0; shift -= 4)
        UARTCharPut(UART0_BASE, hex[(cmd >> shift) & 15U]);
    demo_puts("\r\n");
}

static void demo_rx_log(const uint8_t *rx, uint8_t len)
{
    uint8_t i;
    demo_puts("RX data=\"");
    for (i = 0U; i < len; ++i)
        UARTCharPut(UART0_BASE, rx[i] >= 32U && rx[i] <= 126U ? rx[i] : '.');
    demo_puts("\" len="); demo_u32(len);
    demo_puts(" CRC_OK count="); demo_u32(g_rf433_rx_count);
    demo_puts("\r\n");
}

int main(void)
{
    uint8_t rx[32];
    uint8_t len;
    rfp_status_t status;
    demo_uart_init();
    demo_puts("\r\nCC1310 RF433 BOARD="); demo_u32(RF433_BOARD);
    demo_puts(" FREQ=433.92MHz RATE=50kbps UART=115200\r\n");
    status = RFP_Init();
    g_rf433_init_result = status;
    if (status != RFP_OK)
    {
        for (;;)
        {
            demo_error("RF INIT FAIL", status);
            demo_delay_ms(2000U);
        }
    }
    demo_puts("RF READY\r\n");

    for (;;)
    {
#if RF433_BOARD == 1
        status = RFP_Send((const uint8_t *)"123", 3U);
        if (status != RFP_OK)
        {
            ++g_rf433_error_count;
            demo_error("TX FAIL", status);
            demo_delay_ms(500U);
            continue;
        }
        ++g_rf433_tx_count;
        /* Listen immediately after TX, before printing a long UART log. */
        status = RFP_Receive(rx, sizeof(rx), &len, 1000U);
        demo_puts("TX data=\"123\" count="); demo_u32(g_rf433_tx_count);
        demo_puts("\r\n");
        if (status == RFP_OK)
        {
            ++g_rf433_rx_count;
            demo_rx_log(rx, len);
            demo_puts(len == 3U && memcmp(rx, "888", 3U) == 0 ?
                      "ROUNDTRIP PASS\r\n" : "RX UNEXPECTED\r\n");
        }
        else if (status == RFP_ERR_RX_TIMEOUT)
        {
            ++g_rf433_timeout_count;
            demo_puts("RX TIMEOUT waiting for 888\r\n");
        }
        else
        {
            ++g_rf433_error_count;
            demo_error("RX FAIL", status);
        }
        demo_delay_ms(200U);
#else
        status = RFP_Receive(rx, sizeof(rx), &len, 1000U);
        if (status == RFP_ERR_RX_TIMEOUT)
        {
            if (++g_rf433_timeout_count % 5U == 0U)
                demo_puts("RX WAITING for 123\r\n");
            continue;
        }
        if (status != RFP_OK)
        {
            ++g_rf433_error_count;
            demo_error("RX FAIL", status);
            demo_delay_ms(100U);
            continue;
        }
        ++g_rf433_rx_count;
        demo_rx_log(rx, len);
        if (len != 3U || memcmp(rx, "123", 3U) != 0)
        {
            demo_puts("RX UNEXPECTED\r\n");
            continue;
        }
        /* Give the initiating board time to enter RX. */
        demo_delay_ms(20U);
        status = RFP_Send((const uint8_t *)"888", 3U);
        if (status == RFP_OK)
        {
            ++g_rf433_tx_count;
            demo_puts("TX data=\"888\" count="); demo_u32(g_rf433_tx_count);
            demo_puts("\r\n");
        }
        else
        {
            ++g_rf433_error_count;
            demo_error("TX FAIL", status);
        }
#endif
    }
}

#endif /* RF433_DEMO_IMPL_H */
