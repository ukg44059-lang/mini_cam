/* STM32 SPI2 -> SSI0 -> XDS110 UART. See ../../SPI_IMAGE_LINK.md.
 * One bounded packet at a time, flow-controlled by DIO8; no full-frame RAM. */
#include <stdint.h>
#include <stdbool.h>
#include <driverlib/prcm.h>
#include <driverlib/ioc.h>
#include <driverlib/gpio.h>
#include <driverlib/ssi.h>
#include <driverlib/uart.h>
#include <driverlib/cpu.h>
#include <inc/hw_memmap.h>

volatile uint32_t g_bridge_packets, g_bridge_bytes, g_bridge_bad, g_bridge_partial;
static uint8_t packet[258];

static void spi_arm(void)
{
  uint32_t d;
  SSIDisable(SSI0_BASE);
  SSIConfigSetExpClk(SSI0_BASE, 48000000, SSI_FRF_MOTO_MODE_1,
                    SSI_MODE_SLAVE, 625000, 8);
  while (SSIDataGetNonBlocking(SSI0_BASE, &d)) { }
  SSIIntClear(SSI0_BASE, SSI_RXOR | SSI_RXTO);
  SSIEnable(SSI0_BASE);
  /* TX FIFO can retain bytes across packets, so refill continuously below.
   * Signature is diagnostic only; frame checksum validates the forward path. */
  for (unsigned i = 0; i < 8; ++i)
    (void)SSIDataPutNonBlocking(SSI0_BASE, "CC13"[i & 3]);
  GPIO_setDio(8);
}

int main(void)
{
  PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL);
  while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON ||
         PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) != PRCM_DOMAIN_POWER_ON) { }
  PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
  PRCMPeripheralRunEnable(PRCM_PERIPH_SSI0);
  PRCMLoadSet(); while (!PRCMLoadGet()) { }
  IOCPinTypeUart(UART0_BASE, IOID_2, IOID_1, IOID_UNUSED, IOID_UNUSED);
  UARTConfigSetExpClk(UART0_BASE, 48000000, 115200,
                     UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
  UARTEnable(UART0_BASE);
  IOCPortConfigureSet(IOID_8, IOC_PORT_GPIO, IOC_STD_OUTPUT);
  GPIO_clearDio(8); GPIO_setOutputEnableDio(8, GPIO_OUTPUT_ENABLE);
  IOCPinTypeSsiSlave(SSI0_BASE, IOID_7, IOID_6, IOID_5, IOID_9);
  for (;;)
  {
    uint32_t got = 0, idle = 0, d;
    spi_arm();
    while (got < sizeof(packet))
    {
      if (SSIDataGetNonBlocking(SSI0_BASE, &d))
      {
        packet[got++] = (uint8_t)d;
        (void)SSIDataPutNonBlocking(SSI0_BASE, "CC13"[(got + 7) & 3]);
        idle = 0;
      }
      else if (got && ++idle > 2000000U) break;
    }
    GPIO_clearDio(8);
    /* Hold low long enough for STM32 to observe the acknowledgement. */
    CPUdelay(16000);
    if (got != sizeof(packet)) { ++g_bridge_partial; continue; }
    unsigned n = packet[0] | ((unsigned)packet[1] << 8);
    if (n == 0 || n > 256 || (SSIIntStatus(SSI0_BASE, false) & SSI_RXOR))
    { ++g_bridge_bad; continue; }
    for (unsigned i = 0; i < n; ++i) UARTCharPut(UART0_BASE, packet[i + 2]);
    while (UARTBusy(UART0_BASE)) { }
    ++g_bridge_packets; g_bridge_bytes += n;
  }
}
