#ifndef UART_TEST_IMPL_H
#define UART_TEST_IMPL_H

#include <stdint.h>

#include <driverlib/cpu.h>
#include <driverlib/gpio.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/uart.h>
#include <inc/hw_memmap.h>

#ifndef UART_TEST_TX_DIO
#error "Define UART_TEST_TX_DIO before including this file"
#endif
#ifndef UART_TEST_RX_DIO
#error "Define UART_TEST_RX_DIO before including this file"
#endif
#ifndef UART_TEST_TEXT
#error "Define UART_TEST_TEXT before including this file"
#endif

#define UART_TEST_CLOCK_HZ  48000000U
#define UART_TEST_BAUD      115200U

/* Readable through JTAG: verify that the application transmits and that
 * the TX pad is observed at both logic levels during a UART frame. */
volatile uint32_t g_uart_test_lines_sent;
volatile uint32_t g_uart_test_tx_low_samples;
volatile uint32_t g_uart_test_tx_high_samples;

static void uart_test_init(void)
{
    /* GPIO belongs to PERIPH, but UART0 belongs to SERIAL. Both domains
     * must be powered before accessing the UART and IOC registers. */
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL);
    while ((PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON) ||
           (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) != PRCM_DOMAIN_POWER_ON))
    {
    }

    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
    PRCMLoadSet();
    while (!PRCMLoadGet())
    {
    }

    IOCPinTypeUart(UART0_BASE, UART_TEST_RX_DIO, UART_TEST_TX_DIO,
                   IOID_UNUSED, IOID_UNUSED);
    IOCIOInputSet(UART_TEST_TX_DIO, IOC_INPUT_ENABLE);
    UARTDisable(UART0_BASE);
    UARTConfigSetExpClk(UART0_BASE, UART_TEST_CLOCK_HZ, UART_TEST_BAUD,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                        UART_CONFIG_PAR_NONE);
    UARTEnable(UART0_BASE);
}

int main(void)
{
    static const char message[] = UART_TEST_TEXT "\r\n";
    uint32_t i;

    uart_test_init();

    for (;;)
    {
        for (i = 0U; i < (sizeof(message) - 1U); ++i)
        {
            UARTCharPut(UART0_BASE, (uint8_t)message[i]);
        }

        while (UARTBusy(UART0_BASE))
        {
            if (GPIO_readDio(UART_TEST_TX_DIO))
            {
                ++g_uart_test_tx_high_samples;
            }
            else
            {
                ++g_uart_test_tx_low_samples;
            }
        }
        ++g_uart_test_lines_sent;

        /* CPUdelay executes about 3 cycles per iteration at the default
         * 48 MHz clock: 8,000,000 iterations is approximately 0.5 s. */
        CPUdelay(8000000U);
    }
}

#endif /* UART_TEST_IMPL_H */
