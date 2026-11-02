/* Board 2: 433.92 MHz -> framed records on UART -> host image reassembly.
 * Other board's existing XDS wiring is DIO5 TX / DIO3 RX (override at build). */
#include <string.h>
#define RF_IMAGE_UART_ASYNC
#define RF_IMAGE_UART_CONTROL
#include "rf_image_io.h"
#include "rf_image_protocol.h"
#include "wireless_control.h"
#include <inc/hw_types.h>
#include <inc/hw_rfc_rat.h>
#ifndef RF_IMAGE_UART_TX
#define RF_IMAGE_UART_TX IOID_5
#endif
#ifndef RF_IMAGE_UART_RX
#define RF_IMAGE_UART_RX IOID_3
#endif
volatile uint32_t g_image_rx_packets,g_image_rx_duplicates,g_image_rx_rejected;
volatile uint32_t g_image_rx_frames,g_image_rx_bytes,g_image_ack_errors;
volatile uint32_t g_image_uart_backpressure;
#ifndef RFI_ACK_GUARD_US
#define RFI_ACK_GUARD_US 2000U
#endif
static uint8_t packet[255],ack[RFI_HEADER];
static rfi_receiver_t receiver;
static struct { uint32_t seq,queued_at; unsigned n; uint8_t data[WC_COMMAND_MAX]; } commands[8];
static unsigned command_head,command_tail;
static uint32_t control_session,control_seq,status_session,status_seq;
static uint8_t host_record[WC_COMMAND_MAX+10],control_reply[WC_HEADER+WC_COMMAND_MAX];
static unsigned host_used;
static void expire_controls(void)
{
  uint32_t now=HWREG(RFC_RAT_BASE+RFC_RAT_O_RATCNT);
  while(command_tail!=command_head && (uint32_t)(now-commands[command_tail].queued_at)>32000000U) {
    command_tail=(command_tail+1U)&7U;
    image_log("[RF433 RX] CONTROL_EXPIRED\r\n");
  }
}
static void read_controls(void)
{
  expire_controls();
  int value;
  while((value=image_read_byte())>=0) {
    host_record[host_used++]=(uint8_t)value;
    while(host_used>=4 && memcmp(host_record,"WRC1",4)) {
      memmove(host_record,host_record+1,--host_used);
    }
    if(host_used<6) continue;
    unsigned n=host_record[4]|((unsigned)host_record[5]<<8);
    if(!n || n>WC_COMMAND_MAX) { host_used=0; continue; }
    if(host_used<n+10U) continue;
    if(WC_Check(host_record,6+n)==WC_Get32(host_record+6+n)) {
      unsigned next=(command_head+1U)&7U;
      if(next==command_tail) image_log("[RF433 RX] CONTROL_QUEUE_FULL\r\n");
      else {
        commands[command_head].seq=++control_seq;
        commands[command_head].queued_at=HWREG(RFC_RAT_BASE+RFC_RAT_O_RATCNT);
        commands[command_head].n=n;
        memcpy(commands[command_head].data,host_record+6,n);
        command_head=next;
      }
    }
    host_used=0;
  }
}
static void wireless_packet(unsigned n)
{
  expire_controls();
  uint32_t peer=WC_Get32(packet+4),seq=WC_Get32(packet+8);
  unsigned size=packet[12]|((unsigned)packet[13]<<8),reply_size=0;
  if(packet[3]==WC_POLL) {
    if(command_tail!=command_head && WC_Get32(packet+16)==control_session &&
       WC_Get32(packet+20)==commands[command_tail].seq) command_tail=(command_tail+1U)&7U;
    if(command_tail!=command_head)
      reply_size=WC_Encode(control_reply,WC_REPLY,control_session,commands[command_tail].seq,
                          commands[command_tail].data,commands[command_tail].n);
    else reply_size=WC_Encode(control_reply,WC_REPLY,control_session,0,0,0);
  } else if(packet[3]==WC_STATUS) {
    if(peer!=status_session || seq!=status_seq) {
      if(image_uart_free()<size+10U+160U) return; /* retry without losing status */
      uint8_t prefix[6]={'W','T','S','1',(uint8_t)size,0},sum[4];
      uint32_t check=2166136261U;
      for(unsigned i=0;i<6;++i) check=(check^prefix[i])*16777619U;
      for(unsigned i=0;i<size;++i) check=(check^packet[16+i])*16777619U;
      WC_Put32(sum,check);
      image_uart(prefix,6);image_uart(packet+16,size);image_uart(sum,4);
      status_session=peer;status_seq=seq;
    }
    reply_size=WC_Encode(control_reply,WC_ACK,peer,seq,0,0);
  }
  if(reply_size) {
    CPUdelay(16U*RFI_ACK_GUARD_US);
    if(RFP_Send(control_reply,(uint8_t)reply_size)!=RFP_OK) ++g_image_ack_errors;
  }
  (void)n;
}
int main(void)
{
  image_io_init(RF_IMAGE_UART_TX,RF_IMAGE_UART_RX);
  image_log("[RF433 RX] BOOT\r\n"); image_radio_init();
  control_session=HWREG(RFC_RAT_BASE+RFC_RAT_O_RATCNT)|1U;
  image_log("[RF433 RX] READY waiting_for_radio\r\n");
  for(;;) {
    read_controls();
    uint8_t n=0;
    rfi_packet_t p;
    rfp_status_t rc=RFP_Receive(packet,255,&n,15);
    if(rc==RFP_ERR_RX_TIMEOUT) continue;
    if(rc==RFP_OK && WC_Valid(packet,n)) { wireless_packet(n); continue; }
    if(rc!=RFP_OK || !RFI_Decode(packet,n,&p)) { ++g_image_rx_rejected; continue; }
    /* Commit acceptance only after the complete UART record has queue space.
     * Withhold ACK on pressure; retries are safe and do not duplicate output. */
    rfi_receiver_t next=receiver;
    int accepted=RFI_Accept(&next,&p);
    if(!accepted) { ++g_image_rx_rejected; continue; }
    if(accepted==1 && image_uart_free()<(unsigned)n+10U+160U) {
      ++g_image_uart_backpressure; continue;
    }
    receiver=next;
    if(accepted==1) {
      uint8_t prefix[6]={'R','F','I','1',n,0},sum[4];
      RFI_Put32(sum,RFI_Sum(packet,n));
      image_uart(prefix,6); image_uart(packet,n); image_uart(sum,4);
      ++g_image_rx_packets; g_image_rx_bytes+=p.length;
    } else ++g_image_rx_duplicates;
    if(accepted==1 && (p.flags&RFI_END)) {
      char msg[128]; ++g_image_rx_frames;
      if(g_image_rx_frames==1 || !(g_image_rx_frames%16)) {
      image_format(msg,sizeof(msg),"[RF433 RX] FRAME_OK frame=%lu bytes=%lu frames=%lu duplicates=%lu queue=%u pressure=%lu\r\n",
        (unsigned long)p.frame,(unsigned long)receiver.next,(unsigned long)g_image_rx_frames,
        (unsigned long)g_image_rx_duplicates,g_image_uart_peak,
        (unsigned long)g_image_uart_backpressure); image_log(msg);
      }
    }
    /* UART IRQ drains the bounded queue during RF operations. Allow the peer
     * to enter ACK RX; this guard is separately benchmarked per PHY. */
    CPUdelay(16U*RFI_ACK_GUARD_US);
    p.type=RFI_ACK; RFI_Encode(ack,&p);
    if(RFP_Send(ack,RFI_HEADER)!=RFP_OK) ++g_image_ack_errors;
  }
}
