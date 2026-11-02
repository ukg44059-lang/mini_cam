/* Camera STM32 SPI2 -> CC1310 SSI0 -> 433.92 MHz acknowledged image packets.
 * UART DIO1 TX / DIO2 RX is status only; it never impersonates an RF receiver. */
#include <string.h>
#include "rf_image_io.h"
#include "rf_image_protocol.h"
#include <driverlib/ssi.h>
#include <inc/hw_types.h>
#include <inc/hw_rfc_rat.h>

volatile uint32_t g_image_spi_packets,g_image_spi_bad,g_image_frames_in;
volatile uint32_t g_image_rf_sent,g_image_rf_acks,g_image_rf_retries;
volatile uint32_t g_image_frames_ok,g_image_frames_dropped;
static uint8_t spi_packet[258],radio_packet[RFI_PACKET_MAX],ack_packet[255];
static uint8_t pending[RFI_DATA_MAX];
static unsigned pending_n;
static uint32_t input_offset;
static uint32_t session,frame,offset,total;
static bool failed;
static void spi_arm(void)
{
  uint32_t d;
  SSIDisable(SSI0_BASE);
  /* Slave receives the external SCK; nominal value documents 2.5 MHz operation. */
  SSIConfigSetExpClk(SSI0_BASE,48000000,SSI_FRF_MOTO_MODE_1,SSI_MODE_SLAVE,2500000,8);
  while(SSIDataGetNonBlocking(SSI0_BASE,&d)) { }
  SSIIntClear(SSI0_BASE,SSI_RXOR|SSI_RXTO); SSIEnable(SSI0_BASE);
  for(unsigned i=0;i<8;++i) (void)SSIDataPutNonBlocking(SSI0_BASE,"CC13"[i&3]);
  GPIO_setDio(8);
}
static bool send_fragment(const uint8_t *data, unsigned n, uint8_t flags)
{
  rfi_packet_t p={RFI_DATA,flags,session,frame,offset,(uint16_t)n,data},ack;
  unsigned size=RFI_Encode(radio_packet,&p);
  /* Three attempts: normal worst-case for two fragments is below STM32's
   * 1 s READY timeout. No peer drops this frame, then SPI continues draining. */
  for(unsigned attempt=0;attempt<3;++attempt) {
    uint8_t got=0;
    if(attempt) ++g_image_rf_retries;
    if(RFP_Send(radio_packet,(uint8_t)size)!=RFP_OK) continue;
    ++g_image_rf_sent;
    if(RFP_Receive(ack_packet,255,&got,65)==RFP_OK &&
       RFI_Decode(ack_packet,got,&ack) && ack.type==RFI_ACK &&
       ack.session==session && ack.frame==frame && ack.offset==offset &&
       ack.length==n && ack.flags==flags) {
      ++g_image_rf_acks;
      /* Receiver must rearm RX after finishing ACK TX before our next DATA. */
      CPUdelay(3200); /* 200 us at 48 MHz */
      return true;
    }
  }
  return false;
}
static void process(const uint8_t *p, unsigned n)
{
  char msg[160];
  if(n==14 && !memcmp(p,"OV3S",4)) {
    uint32_t len=RFI_Get32(p+10);
    unsigned w=p[4]|((unsigned)p[5]<<8),h=p[6]|((unsigned)p[7]<<8);
    if(!w || !h || w>2048 || h>2048 || (w&3) || (h&3) ||
       p[8]>2 || !len || len>1500000U ||
       (p[8] && len!=(uint32_t)w*h*p[8])) { total=0; ++g_image_spi_bad; return; }
    if(!session) session=HWREG(RFC_RAT_BASE+RFC_RAT_O_RATCNT)|1U;
    ++frame; ++g_image_frames_in; offset=0; input_offset=0; pending_n=0;
    total=len+18; failed=false;
  }
  if(!total || n>total-input_offset) { ++g_image_spi_bad; total=0; return; }
  for(unsigned pos=0;pos<n;) {
    unsigned take=n-pos;
    if(take>RFI_DATA_MAX-pending_n) take=RFI_DATA_MAX-pending_n;
    memcpy(pending+pending_n,p+pos,take);
    pending_n+=take; input_offset+=take; pos+=take;
    if(pending_n<RFI_DATA_MAX && input_offset<total) continue;
    uint8_t flags=(offset==0 ? RFI_START:0) | (input_offset==total ? RFI_END:0);
    if(!failed && !send_fragment(pending,pending_n,flags)) {
      failed=true; ++g_image_frames_dropped;
      /* Throttle missing-peer messages while keeping counters current. */
      if(g_image_frames_dropped<=3 || !(g_image_frames_dropped%16)) {
        image_format(msg,sizeof(msg),"[RF433 TX] NO_ACK frame=%lu offset=%lu input=%lu sent=%lu ack=%lu dropped=%lu status=0x%04x\r\n",
          (unsigned long)frame,(unsigned long)offset,(unsigned long)g_image_frames_in,
          (unsigned long)g_image_rf_sent,(unsigned long)g_image_rf_acks,
          (unsigned long)g_image_frames_dropped,RFP_LastCmdStatus()); image_log(msg);
      }
    }
    offset+=pending_n; pending_n=0;
  }
  if(offset==total) {
    if(!failed) {
      ++g_image_frames_ok;
      if(g_image_frames_ok==1 || !(g_image_frames_ok%16)) {
      image_format(msg,sizeof(msg),"[RF433 TX] FRAME_OK frame=%lu bytes=%lu frames_ok=%lu retries=%lu\r\n",
        (unsigned long)frame,(unsigned long)total,(unsigned long)g_image_frames_ok,
        (unsigned long)g_image_rf_retries); image_log(msg);
      }
    }
    total=0;
  }
}
int main(void)
{
  image_io_init(IOID_1,IOID_2);
  IOCPortConfigureSet(IOID_8,IOC_PORT_GPIO,IOC_STD_OUTPUT);
  GPIO_clearDio(8); GPIO_setOutputEnableDio(8,GPIO_OUTPUT_ENABLE);
  PRCMPeripheralRunEnable(PRCM_PERIPH_SSI0); PRCMLoadSet(); while(!PRCMLoadGet()) { }
  IOCPinTypeSsiSlave(SSI0_BASE,IOID_7,IOID_6,IOID_5,IOID_9);
  image_log("[RF433 TX] BOOT uart_tx=DIO1 uart_rx=DIO2\r\n");
  image_radio_init(); image_log("[RF433 TX] READY waiting_for_camera_and_receiver\r\n");
  for(;;) {
    uint32_t got=0,idle=0,d;
    spi_arm();
    while(got<sizeof(spi_packet)) {
      if(SSIDataGetNonBlocking(SSI0_BASE,&d)) {
        spi_packet[got++]=(uint8_t)d;
        (void)SSIDataPutNonBlocking(SSI0_BASE,"CC13"[(got+7)&3]); idle=0;
      } else if(got && ++idle>2000000U) break;
    }
    GPIO_clearDio(8); CPUdelay(1600); /* 100 us, observable READY-low pulse */
    unsigned n=spi_packet[0]|((unsigned)spi_packet[1]<<8);
    if(got!=sizeof(spi_packet) || !n || n>256 || (SSIIntStatus(SSI0_BASE,false)&SSI_RXOR))
    { ++g_image_spi_bad; total=0; continue; }
    ++g_image_spi_packets; process(spi_packet+2,n);
  }
}
