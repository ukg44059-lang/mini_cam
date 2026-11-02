/* Custom board: SPI2 AF5, with MOSI/MISO exchanged by IOSWP.
 * PD14 goes to RESET_N, NOT a CC1310 ready output. DIO8/PD15 is ready.
 * Wire format: 258 bytes per CS: little-endian length + 256 padded bytes.
 * DIO8 high = armed; low = packet received / UART draining. */
#include "bsp.h"
#include "cc1310_link.h"
#include <string.h>
#include <stdbool.h>

static SPI_HandleTypeDef spi;
volatile uint32_t g_cc_packets, g_cc_timeouts, g_cc_signatures;
volatile uint32_t g_cc_spi_errors, g_cc_last_status, g_cc_last_error;

void CC_LinkInit(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_SPI2_CLK_ENABLE();
  __HAL_RCC_SPI2_CONFIG(RCC_SPI2CLKSOURCE_SYSCLK);
  /* PB15 is also UCPD CC2: remove its dead-battery pull-down for SPI. */
  HAL_PWREx_DisableUCPDDeadBattery();
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_9, GPIO_PIN_SET);
  g.Pin = GPIO_PIN_9; g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOE, &g);
  /* Release RESET_N via open drain; never drive a high against XDS110. */
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET);
  g.Pin = GPIO_PIN_14; g.Mode = GPIO_MODE_OUTPUT_OD;
  HAL_GPIO_Init(GPIOD, &g);
  g.Pin = GPIO_PIN_15; g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOD, &g);
  g.Pin = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
  g.Mode = GPIO_MODE_AF_PP; g.Pull = GPIO_NOPULL;
  g.Alternate = GPIO_AF5_SPI2;
  HAL_GPIO_Init(GPIOB, &g);
  spi.Instance = SPI2;
  spi.Init.Mode = SPI_MODE_MASTER;
  spi.Init.Direction = SPI_DIRECTION_2LINES;
  spi.Init.DataSize = SPI_DATASIZE_8BIT;
  spi.Init.CLKPolarity = SPI_POLARITY_LOW;
  /* CC1310 SSI needs mode 1 to accept a multi-byte burst under one CS. */
  spi.Init.CLKPhase = SPI_PHASE_2EDGE;
  spi.Init.NSS = SPI_NSS_SOFT;
  spi.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64; /* SYSCLK160/64=2.5MHz */
  spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
  spi.Init.TIMode = SPI_TIMODE_DISABLE;
  spi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  spi.Init.IOSwap = SPI_IO_SWAP_ENABLE; /* PB14 out, PB15 in */
  spi.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
  if (HAL_SPI_Init(&spi) != HAL_OK) Error_Handler();
}

static bool wait_ready(GPIO_PinState state)
{
  uint32_t start = HAL_GetTick();
  while (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_15) != state)
    if ((HAL_GetTick() - start) > 1000U) { ++g_cc_timeouts; return false; }
  return true;
}

void CC_LinkSetSpeed(unsigned level)
{
  spi.Init.BaudRatePrescaler = level==2U ? SPI_BAUDRATEPRESCALER_64 :
                              level==1U ? SPI_BAUDRATEPRESCALER_128 :
                                          SPI_BAUDRATEPRESCALER_256;
  if (HAL_SPI_Init(&spi) != HAL_OK) Error_Handler();
}

uint32_t CC_LinkWrite(const uint8_t *data, uint32_t len)
{
  uint32_t done = 0;
  uint8_t tx[258], rx[258];
  while (done < len)
  {
    uint32_t n = len - done;
    if (n > 256U) n = 256U;
    memset(tx, 0, sizeof(tx));
    tx[0] = (uint8_t)n; tx[1] = (uint8_t)(n >> 8);
    memcpy(tx + 2, data + done, n);
    if (!wait_ready(GPIO_PIN_SET)) return done;
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_9, GPIO_PIN_RESET);
    HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&spi, tx, rx, sizeof(tx), 200);
    g_cc_last_status = st;
    g_cc_last_error = spi.ErrorCode;
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_9, GPIO_PIN_SET);
    if (st != HAL_OK) { ++g_cc_spi_errors; return done; }
    if (!wait_ready(GPIO_PIN_RESET)) return done;
    /* SSI TX FIFO retains a signature phase between packets. */
    for (unsigned i = 0; i < 8; ++i)
      if (memcmp(rx + i, "CC13", 4) == 0) { ++g_cc_signatures; break; }
    ++g_cc_packets;
    done += n;
  }
  return done;
}
