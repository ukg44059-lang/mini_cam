/* Board 2: 433.92 MHz -> framed records on UART -> host image reassembly.
 * Other board's existing XDS wiring is DIO5 TX / DIO3 RX (override at build). */
#include <string.h>
#define RF_IMAGE_UART_ASYNC
#include "rf_image_io.h"
#include "rf_image_protocol.h"
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
int main(void)
{
  image_io_init(RF_IMAGE_UART_TX,RF_IMAGE_UART_RX);
  image_log("[RF433 RX] BOOT\r\n"); image_radio_init();
  image_log("[RF433 RX] READY waiting_for_radio\r\n");
  for(;;) {
    uint8_t n=0;
    rfi_packet_t p;
    rfp_status_t rc=RFP_Receive(packet,255,&n,250);
    if(rc==RFP_ERR_RX_TIMEOUT) continue;
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
