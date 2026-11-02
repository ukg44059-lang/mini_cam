/* Custom board: SPI2 AF5, with MOSI/MISO exchanged by IOSWP.
 * PD14 goes to RESET_N, NOT a CC1310 ready output. DIO8/PD15 is ready.
 * Wire format: 258 bytes per CS: little-endian length + 256 padded bytes.
 * DIO8 high = armed; low = packet received / UART draining. */
#include "bsp.h"
#include "cc1310_link.h"
#include <string.h>
#include <stdbool.h>
#include "../../cc1310/Core/Inc/wireless_control.h"

static SPI_HandleTypeDef spi;
volatile uint32_t g_cc_packets, g_cc_timeouts, g_cc_signatures;
volatile uint32_t g_cc_spi_errors, g_cc_last_status, g_cc_last_error;
static uint8_t command_queue[4][WC_COMMAND_MAX];
static uint16_t command_sizes[4];
static uint32_t command_sessions[4],command_sequences[4];
static unsigned command_head,command_tail;
static uint32_t poll_due;
static uint32_t delivered_session,delivered_sequence;

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

/* One checked mailbox shared by image transfers, status and idle polls. */
static void accept_reply(const uint8_t *rx,unsigned size)
{
  static uint32_t last_session,last_seq;
  for(unsigned off=0;off+20U<=size;++off) {
    const uint8_t *p=rx+off;
    if(memcmp(p,"WC13",4)) continue;
    unsigned n=p[12]|((unsigned)p[13]<<8);
    if(n>WC_COMMAND_MAX || p[14] || p[15] || off+20U+n>size) continue;
    if(WC_Check(p,16U+n)!=WC_Get32(p+16U+n)) continue;
    uint32_t session=WC_Get32(p+4),seq=WC_Get32(p+8);
    unsigned next=(command_head+1U)&3U;
    if(n && next!=command_tail && (session!=last_session || seq!=last_seq)) {
      memcpy(command_queue[command_head],p+16,n);
      command_sizes[command_head]=(uint16_t)n;
      command_sessions[command_head]=session;command_sequences[command_head]=seq;
      command_head=next;last_session=session;last_seq=seq;
    }
    ++g_cc_signatures;
    break;
  }
}
static bool exchange(const uint8_t *data,uint32_t len,uint16_t tag)
{
  uint8_t tx[258]={0},rx[258];
  if(len>256U) return false;
  tx[0]=(uint8_t)tag;tx[1]=(uint8_t)(tag>>8);
  if(len) memcpy(tx+2,data,len);
  if(!wait_ready(GPIO_PIN_SET)) return false;
  HAL_GPIO_WritePin(GPIOE,GPIO_PIN_9,GPIO_PIN_RESET);
  HAL_StatusTypeDef st=HAL_SPI_TransmitReceive(&spi,tx,rx,sizeof(tx),200);
  g_cc_last_status=st;g_cc_last_error=spi.ErrorCode;
  HAL_GPIO_WritePin(GPIOE,GPIO_PIN_9,GPIO_PIN_SET);
  if(st!=HAL_OK) { ++g_cc_spi_errors;return false; }
  if(!wait_ready(GPIO_PIN_RESET)) return false;
  accept_reply(rx,sizeof(rx));++g_cc_packets;
  return true;
}
uint32_t CC_LinkWrite(const uint8_t *data,uint32_t len)
{
  uint32_t done=0;
  while(done<len) {
    uint32_t n=len-done;if(n>256U)n=256U;
    if(!exchange(data+done,n,(uint16_t)n)) break;
    done+=n;
  }
  return done;
}
uint32_t CC_LinkRead(uint8_t *data,uint32_t capacity)
{
  if(command_tail==command_head) return 0;
  uint32_t n=command_sizes[command_tail];
  if(n>capacity) return 0; /* Never execute a truncated command batch. */
  memcpy(data,command_queue[command_tail],n);
  delivered_session=command_sessions[command_tail];delivered_sequence=command_sequences[command_tail];
  command_tail=(command_tail+1U)&3U;poll_due=HAL_GetTick();return n;
}
uint32_t CC_LinkTelemetry(const uint8_t *data,uint32_t len)
{
  if(!len || len>WC_STATUS_MAX) return 0;
  return exchange(data,len,(uint16_t)(0x8000U|len))?len:0;
}
uint32_t CC_LinkPoll(void)
{
  uint32_t now=HAL_GetTick();if((int32_t)(now-poll_due)<0)return 0;
  poll_due=now+100U;
  uint8_t receipt[8];WC_Put32(receipt,delivered_session);WC_Put32(receipt+4,delivered_sequence);
  return exchange(receipt,sizeof(receipt),0x4008U);
}
