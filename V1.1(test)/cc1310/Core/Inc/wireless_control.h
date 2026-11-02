/* Wireless control v1. RF CRC protects air packets; FNV-1a protects UART/SPI
 * records and resynchronization. All integers are little endian. */
#ifndef WIRELESS_CONTROL_H
#define WIRELESS_CONTROL_H
#include <stdint.h>
#include <string.h>
#define WC_HEADER 16U
#define WC_COMMAND_MAX 160U
#define WC_STATUS_MAX 224U
#define WC_POLL 1U
#define WC_REPLY 2U
#define WC_STATUS 3U
#define WC_ACK 4U
static inline uint32_t WC_Get32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static inline void WC_Put32(uint8_t *p,uint32_t v)
{ for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static inline uint32_t WC_Check(const uint8_t *p,unsigned n)
{ uint32_t v=2166136261U; while(n--) v=(v^*p++)*16777619U; return v; }
static inline unsigned WC_Encode(uint8_t *p,unsigned type,uint32_t session,uint32_t seq,const void *data,unsigned n)
{
  p[0]='W';p[1]='C';p[2]=1;p[3]=(uint8_t)type;
  WC_Put32(p+4,session);WC_Put32(p+8,seq);
  p[12]=(uint8_t)n;p[13]=(uint8_t)(n>>8);p[14]=p[15]=0;
  if(n) memcpy(p+WC_HEADER,data,n);
  return WC_HEADER+n;
}
static inline int WC_Valid(const uint8_t *p,unsigned n)
{
  if(n<WC_HEADER || p[0]!='W' || p[1]!='C' || p[2]!=1 || p[14] || p[15]) return 0;
  unsigned size=p[12]|((unsigned)p[13]<<8);
  if(n!=WC_HEADER+size) return 0;
  return (p[3]==WC_POLL && size==8) || (p[3]==WC_REPLY && size<=WC_COMMAND_MAX) ||
         (p[3]==WC_STATUS && size>0 && size<=WC_STATUS_MAX) || (p[3]==WC_ACK && !size);
}
#endif
