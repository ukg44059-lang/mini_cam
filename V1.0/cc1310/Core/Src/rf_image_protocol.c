#include "rf_image_protocol.h"
#include <string.h>
uint32_t RFI_Get32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
void RFI_Put32(uint8_t *p, uint32_t v)
{ for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i)); }
uint32_t RFI_Sum(const uint8_t *p, unsigned n)
{ uint32_t v=0; while(n--) v+=*p++; return v; }
unsigned RFI_Encode(uint8_t *o, const rfi_packet_t *p)
{
  o[0]='R'; o[1]='I'; o[2]=1; o[3]=p->type;
  RFI_Put32(o+4,p->session); RFI_Put32(o+8,p->frame); RFI_Put32(o+12,p->offset);
  o[16]=(uint8_t)p->length; o[17]=(uint8_t)(p->length>>8);
  o[18]=p->flags; o[19]=0;
  if(p->type==RFI_DATA) memcpy(o+RFI_HEADER,p->data,p->length);
  return RFI_HEADER+(p->type==RFI_DATA ? p->length : 0);
}
bool RFI_Decode(const uint8_t *d, unsigned n, rfi_packet_t *p)
{
  if(n<RFI_HEADER || d[0]!='R' || d[1]!='I' || d[2]!=1 ||
     (d[3]!=RFI_DATA && d[3]!=RFI_ACK) || d[19] || (d[18]&~3U)) return false;
  p->type=d[3]; p->session=RFI_Get32(d+4); p->frame=RFI_Get32(d+8);
  p->offset=RFI_Get32(d+12); p->length=d[16]|((uint16_t)d[17]<<8);
  p->flags=d[18]; p->data=d+RFI_HEADER;
  return p->length && p->length<=RFI_DATA_MAX && p->offset<=1500018U &&
         n==RFI_HEADER+(p->type==RFI_DATA ? p->length : 0) &&
         (!(p->flags&RFI_START) || p->offset==0);
}
int RFI_Accept(rfi_receiver_t *s, const rfi_packet_t *p)
{
  uint32_t hash=2166136261U;
  if(p->type!=RFI_DATA) return 0;
  for(unsigned i=0;i<p->length;++i) hash=(hash^p->data[i])*16777619U;
  bool same=s->session==p->session && s->frame==p->frame;
  if(s->have_last && same && s->last_offset==p->offset &&
     s->last_length==p->length && s->last_flags==p->flags && s->last_hash==hash) return 2;
  if(p->flags&RFI_START) {
    s->session=p->session; s->frame=p->frame; s->next=0; s->active=true;
  } else if(!s->active || !same) return 0;
  if(p->offset!=s->next) return 0;
  s->next+=p->length; s->last_offset=p->offset; s->last_length=p->length;
  s->last_flags=p->flags; s->last_hash=hash; s->have_last=true;
  if(p->flags&RFI_END) s->active=false;
  return 1;
}
