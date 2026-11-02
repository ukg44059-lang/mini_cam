#ifndef RF_IMAGE_IO_H
#define RF_IMAGE_IO_H
#include <stdarg.h>
#include <string.h>
#include <driverlib/prcm.h>
#include <driverlib/ioc.h>
#include <driverlib/gpio.h>
#include <driverlib/uart.h>
#include <driverlib/cpu.h>
#include <inc/hw_memmap.h>
#include "rf_prop.h"
#ifndef RF_IMAGE_UART_BAUD
#define RF_IMAGE_UART_BAUD 115200U
#endif
#ifndef RFP_RATE_KBPS
#define RFP_RATE_KBPS 50U
#endif
#ifdef RF_IMAGE_UART_ASYNC
#include <driverlib/interrupt.h>
#include <inc/hw_ints.h>
#define IMAGE_UART_RING 8192U
static uint8_t image_uart_ring[IMAGE_UART_RING];
static volatile unsigned image_uart_head, image_uart_tail;
volatile unsigned g_image_uart_peak;
#ifdef RF_IMAGE_UART_CONTROL
static uint8_t image_rx_ring[512];
static volatile unsigned image_rx_head,image_rx_tail;
volatile unsigned g_image_control_overruns;
static int image_read_byte(void)
{
  if(image_rx_tail==image_rx_head) return -1;
  unsigned value=image_rx_ring[image_rx_tail];
  image_rx_tail=(image_rx_tail+1U)&511U; return (int)value;
}
#endif
static unsigned image_uart_free(void)
{ return (image_uart_tail-image_uart_head-1U)&(IMAGE_UART_RING-1U); }
/* One producer in main, one consumer in the UART TX interrupt. */
void UART0IntHandler(void)
{
  UARTIntClear(UART0_BASE,UART_INT_TX|UART_INT_RX|UART_INT_RT);
#ifdef RF_IMAGE_UART_CONTROL
  while(UARTCharsAvail(UART0_BASE)) {
    int value=UARTCharGetNonBlocking(UART0_BASE);
    unsigned next=(image_rx_head+1U)&511U;
    if(next==image_rx_tail) ++g_image_control_overruns;
    else { image_rx_ring[image_rx_head]=(uint8_t)value; image_rx_head=next; }
  }
#endif
  while(image_uart_tail!=image_uart_head &&
        UARTCharPutNonBlocking(UART0_BASE,image_uart_ring[image_uart_tail]))
    image_uart_tail=(image_uart_tail+1U)&(IMAGE_UART_RING-1U);
  if(image_uart_tail==image_uart_head) UARTIntDisable(UART0_BASE,UART_INT_TX);
}
#endif
#if RFP_BAND != 433
#error Build image radio firmware with BAND=433
#endif
static void image_io_init(unsigned tx, unsigned rx)
{
  PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL);
  while(PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH)!=PRCM_DOMAIN_POWER_ON ||
        PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL)!=PRCM_DOMAIN_POWER_ON) { }
  PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
  PRCMPeripheralRunEnable(PRCM_PERIPH_UART0);
  PRCMLoadSet(); while(!PRCMLoadGet()) { }
  IOCPinTypeUart(UART0_BASE, rx, tx, IOID_UNUSED, IOID_UNUSED);
  UARTConfigSetExpClk(UART0_BASE,48000000,RF_IMAGE_UART_BAUD,
    UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
  UARTEnable(UART0_BASE);
#ifdef RF_IMAGE_UART_ASYNC
  UARTFIFOLevelSet(UART0_BASE,UART_FIFO_TX4_8,UART_FIFO_RX4_8);
#ifdef RF_IMAGE_UART_CONTROL
  UARTFIFOLevelSet(UART0_BASE,UART_FIFO_TX4_8,UART_FIFO_RX1_8);
  UARTIntEnable(UART0_BASE,UART_INT_RX|UART_INT_RT);
#endif
  IntEnable(INT_UART0_COMB);
  IntMasterEnable();
#endif
}
static void image_uart(const uint8_t *p, unsigned n)
{
#ifdef RF_IMAGE_UART_ASYNC
  while(n) {
    unsigned free=image_uart_free();
    if(!free) continue;
    unsigned take=n<free?n:free,head=image_uart_head;
    for(unsigned i=0;i<take;++i) {
      image_uart_ring[head]=*p++; head=(head+1U)&(IMAGE_UART_RING-1U);
    }
    __asm volatile ("dmb" ::: "memory");
    image_uart_head=head; n-=take;
    unsigned used=IMAGE_UART_RING-1U-image_uart_free();
    if(used>g_image_uart_peak) g_image_uart_peak=used;
    /* Mask only UART TX while priming, so ISR and main cannot pop together. */
    IntDisable(INT_UART0_COMB);
    UART0IntHandler();
    UARTIntEnable(UART0_BASE,UART_INT_TX);
    IntEnable(INT_UART0_COMB);
  }
#else
  while(n--) UARTCharPut(UART0_BASE,*p++);
#endif
}
static void image_log(const char *text)
{ image_uart((const uint8_t *)text,(unsigned)strlen(text)); }
/* Only the unsigned formats used by status messages; no libc heap/printf. */
static void image_format(char *out, unsigned capacity, const char *fmt, ...)
{
  va_list ap; va_start(ap,fmt); unsigned pos=0;
  while(*fmt && pos+1<capacity) {
    if(*fmt!='%') { out[pos++]=*fmt++; continue; }
    ++fmt; unsigned width=0,base=10; bool wide=false;
    if(*fmt=='0') { ++fmt; width=(unsigned)(*fmt++-'0'); }
    if(*fmt=='l') { wide=true; ++fmt; }
    if(*fmt=='x') base=16;
    ++fmt;
    uint32_t value=wide ? va_arg(ap,unsigned long) : va_arg(ap,unsigned int);
    char digits[12]; unsigned n=0;
    do { digits[n++]="0123456789abcdef"[value%base]; value/=base; } while(value);
    while(n<width) digits[n++]='0';
    while(n && pos+1<capacity) out[pos++]=digits[--n];
  }
  out[pos]=0; va_end(ap);
}
static void image_radio_init(void)
{
  rfp_status_t rc=RFP_Init();
  char msg[100];
  image_format(msg,sizeof(msg),"[RF433] RF_INIT result=%u status=0x%04x rate=%ukbps uart=%u\r\n",
           (unsigned)rc,RFP_LastCmdStatus(),(unsigned)RFP_RATE_KBPS,(unsigned)RF_IMAGE_UART_BAUD);
  image_log(msg);
  if(rc!=RFP_OK) for(;;) { CPUdelay(16000000); }
}
#endif
