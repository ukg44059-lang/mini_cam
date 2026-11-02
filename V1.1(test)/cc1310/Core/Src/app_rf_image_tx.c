/* Camera STM32 SPI2 -> CC1310 SSI0 -> 433.92 MHz acknowledged image packets.
 * UART DIO1 TX / DIO2 RX is status only; it never impersonates an RF receiver. */
#include <string.h>
#include "rf_image_io.h"
#include "rf_image_protocol.h"
#include "wireless_control.h"
#include <driverlib/ssi.h>
#include <inc/hw_types.h>
#include <inc/hw_rfc_rat.h>

volatile uint32_t g_image_spi_packets,g_image_spi_bad,g_image_frames_in;
volatile uint32_t g_image_rf_sent,g_image_rf_acks,g_image_rf_retries;
volatile uint32_t g_image_frames_ok,g_image_frames_dropped;
volatile uint32_t g_image_ack_timeouts;
static uint8_t spi_packet[258],radio_packet[RFI_PACKET_MAX],ack_packet[255];
static uint8_t pending[RFI_DATA_MAX];
static unsigned pending_n;
static uint32_t input_offset;
static uint32_t session,frame,offset,total;
static bool failed;
static uint8_t spi_reply[258],control_packet[WC_HEADER+WC_STATUS_MAX],control_rx[255];
static unsigned spi_reply_pos;
static uint32_t control_session,control_seq,delivered_session,delivered_seq;
static uint32_t command_session,command_seq;
static unsigned command_size;
static uint8_t command[WC_COMMAND_MAX];
static struct { unsigned n; uint8_t data[WC_STATUS_MAX]; } status_queue[16];
static unsigned status_head,status_tail;
volatile unsigned g_control_status_dropped;
static void queue_status(const uint8_t *data,unsigned n)
{
  unsigned next=(status_head+1U)&15U;
  if(!n || n>WC_STATUS_MAX) return;
  if(next==status_tail) { ++g_control_status_dropped; return; }
  status_queue[status_head].n=n;memcpy(status_queue[status_head].data,data,n);status_head=next;
}
static void tx_log(const char *text)
{ image_log(text);queue_status((const uint8_t *)text,(unsigned)strlen(text)); }
static void control_service(void)
{
  uint8_t n=0,payload[8];
  if(status_tail!=status_head) {
    unsigned size=WC_Encode(control_packet,WC_STATUS,control_session,control_seq,
      status_queue[status_tail].data,status_queue[status_tail].n);
    if(RFP_Send(control_packet,(uint8_t)size)==RFP_OK &&
       RFP_Receive(control_rx,255,&n,35)==RFP_OK && WC_Valid(control_rx,n) &&
       control_rx[3]==WC_ACK && WC_Get32(control_rx+4)==control_session &&
       WC_Get32(control_rx+8)==control_seq) {
      status_tail=(status_tail+1U)&15U;++control_seq;CPUdelay(3200);
    }
  }
  WC_Put32(payload,delivered_session);WC_Put32(payload+4,delivered_seq);
  unsigned size=WC_Encode(control_packet,WC_POLL,control_session,0,payload,8);
  if(RFP_Send(control_packet,(uint8_t)size)==RFP_OK &&
     RFP_Receive(control_rx,255,&n,35)==RFP_OK && WC_Valid(control_rx,n) && control_rx[3]==WC_REPLY) {
    unsigned len=control_rx[12]|((unsigned)control_rx[13]<<8);
    if(len && !command_size) {
      command_session=WC_Get32(control_rx+4);command_seq=WC_Get32(control_rx+8);
      /* A repeated poll reply can arrive after STM32 accepted this command. */
      if(command_session!=delivered_session || command_seq!=delivered_seq) {
        memcpy(command,control_rx+16,len);command_size=len;
      }
    }
    CPUdelay(3200);
  }
}
static void spi_fill(void)
{
  /* SSI keeps up to eight old TX bytes between CS cycles. STM32 scans for the
   * checked WC13 record; never restart the record when the FIFO is full. */
  while(spi_reply_pos<258 && SSIDataPutNonBlocking(SSI0_BASE,spi_reply[spi_reply_pos])) ++spi_reply_pos;
}
static void spi_arm(void)
{
  uint32_t d;
  SSIDisable(SSI0_BASE);
  /* Slave receives the external SCK; nominal value documents 2.5 MHz operation. */
  SSIConfigSetExpClk(SSI0_BASE,48000000,SSI_FRF_MOTO_MODE_1,SSI_MODE_SLAVE,2500000,8);
  while(SSIDataGetNonBlocking(SSI0_BASE,&d)) { }
  SSIIntClear(SSI0_BASE,SSI_RXOR|SSI_RXTO); SSIEnable(SSI0_BASE);
  memset(spi_reply,0,sizeof(spi_reply));memcpy(spi_reply,"WC13",4);
  WC_Put32(spi_reply+4,command_session);WC_Put32(spi_reply+8,command_seq);
  spi_reply[12]=(uint8_t)command_size;spi_reply[13]=(uint8_t)(command_size>>8);
  spi_reply[14]=spi_reply[15]=0;
  if(command_size) memcpy(spi_reply+16,command,command_size);
  WC_Put32(spi_reply+16+command_size,WC_Check(spi_reply,16+command_size));
  spi_reply_pos=0;spi_fill();
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
    ++g_image_ack_timeouts;
  }
  return false;
}
static void process(const uint8_t *p, unsigned n)
{
  char msg[240];
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
        image_format(msg,sizeof(msg),"[RF433 TX] NO_ACK frame=%lu offset=%lu input=%lu sent=%lu ack=%lu dropped=%lu retries=%lu ack_failures=%lu status=0x%04x\r\n",
          (unsigned long)frame,(unsigned long)offset,(unsigned long)g_image_frames_in,
          (unsigned long)g_image_rf_sent,(unsigned long)g_image_rf_acks,
          (unsigned long)g_image_frames_dropped,(unsigned long)g_image_rf_retries,
          (unsigned long)g_image_ack_timeouts,RFP_LastCmdStatus()); tx_log(msg);
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
        (unsigned long)g_image_rf_retries); tx_log(msg);
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
  control_session=HWREG(RFC_RAT_BASE+RFC_RAT_O_RATCNT)|1U;control_seq=1;
  tx_log("[RF433 TX] CONTROL_READY proto=3 transport=rf433\r\n");
  for(;;) {
    uint32_t got=0,idle=0,d;
    spi_arm();
    while(got<sizeof(spi_packet)) {
      if(SSIDataGetNonBlocking(SSI0_BASE,&d)) {
        spi_packet[got++]=(uint8_t)d;
        spi_fill(); idle=0;
      } else if(got && ++idle>2000000U) break;
    }
    GPIO_clearDio(8); CPUdelay(1600); /* 100 us, observable READY-low pulse */
    unsigned n=spi_packet[0]|((unsigned)spi_packet[1]<<8);
    if(got!=sizeof(spi_packet) || (SSIIntStatus(SSI0_BASE,false)&SSI_RXOR))
    { ++g_image_spi_bad; total=0; continue; }
    ++g_image_spi_packets;
    if(n==0x4008U) {
      if(WC_Get32(spi_packet+2)) { delivered_session=WC_Get32(spi_packet+2);delivered_seq=WC_Get32(spi_packet+6); }
      if(command_size && WC_Get32(spi_packet+2)==command_session && WC_Get32(spi_packet+6)==command_seq) {
        delivered_session=command_session;delivered_seq=command_seq;command_size=0;
      }
    } else if((n&0x8000U) && (n&0x7fffU)<=WC_STATUS_MAX) queue_status(spi_packet+2,n&0x7fffU);
    else if(n<=256) process(spi_packet+2,n);
    else { ++g_image_spi_bad;total=0; }
    if(!total || n==0x4008U || (n&0x8000U)) control_service();
  }
}
