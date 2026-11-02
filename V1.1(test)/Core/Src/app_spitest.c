/* Wiring diagnostic for the same custom-board link as TRANSPORT=dual. */
#include "bsp.h"
#include "cc1310_link.h"
extern volatile uint32_t g_cc_packets, g_cc_timeouts, g_cc_spi_errors;
extern volatile uint32_t g_cc_last_status, g_cc_last_error;
int main(void)
{
  static const uint8_t text[] = "SPI2 bridge test\r\n";
  BSP_Init(); CC_LinkInit();
  for (;;)
  {
    uint32_t n = CC_LinkWrite(text, sizeof(text)-1);
    printf("SPI2 n=%lu ready=%d packets=%lu timeouts=%lu spi_errors=%lu status=%lu error=%lu\r\n",
      (unsigned long)n, HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_15),
      (unsigned long)g_cc_packets, (unsigned long)g_cc_timeouts,
      (unsigned long)g_cc_spi_errors, (unsigned long)g_cc_last_status,
      (unsigned long)g_cc_last_error);
    HAL_Delay(1000);
  }
}
