#include "rf_image_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void record(FILE *f, const uint8_t *packet, unsigned n)
{
  uint8_t pre[6]={'R','F','I','1',(uint8_t)n,0},sum[4];
  RFI_Put32(sum,RFI_Sum(packet,n));
  fwrite(pre,1,6,f); fwrite(packet,1,n,f); fwrite(sum,1,4,f);
}
int main(int argc, char **argv)
{
  assert(argc==2);
  uint8_t wire[RFI_PACKET_MAX],data[RFI_DATA_MAX];
  memset(data,42,sizeof(data));
  rfi_receiver_t rx={0};
  rfi_packet_t p={RFI_DATA,RFI_START,123,1,0,224,data},q;
  unsigned n=RFI_Encode(wire,&p);
  assert(n==244 && RFI_Decode(wire,n,&q));
  assert(RFI_Accept(&rx,&q)==1);
  assert(RFI_Accept(&rx,&q)==2); /* lost ACK: retransmit without UART duplicate */
  q.offset=448; q.flags=0; assert(RFI_Accept(&rx,&q)==0); /* missing fragment */
  q.offset=224; q.flags=RFI_END; assert(RFI_Accept(&rx,&q)==1);
  assert(RFI_Accept(&rx,&q)==2); /* lost final ACK */
  q.offset=448; q.flags=0; assert(RFI_Accept(&rx,&q)==0); /* already ended */
  p.type=RFI_ACK; n=RFI_Encode(wire,&p);
  assert(n==20 && RFI_Decode(wire,n,&q) && q.length==224 && q.type==RFI_ACK);
  assert(RFI_Accept(&rx,&q)==0);
  wire[18]=0x80; assert(!RFI_Decode(wire,n,&q));
  p.type=RFI_DATA; n=RFI_Encode(wire,&p);
  assert(!RFI_Decode(wire,n-1,&q));
  wire[16]=225; assert(!RFI_Decode(wire,n,&q));
  p.session=124; n=RFI_Encode(wire,&p); assert(RFI_Decode(wire,n,&q));
  assert(RFI_Accept(&rx,&q)==1); /* sender reboot/session reset */

  /* Real C encoder + duplicate filter -> host UART fixture. The first frame
   * aborts; the next 19,200-byte frame must survive arbitrary UART chunking. */
  FILE *f=fopen(argv[1],"wb"); assert(f);
  uint8_t image[19218]={ 'O','V','3','S',160,0,120,0,1,7 };
  RFI_Put32(image+10,19200);
  for(unsigned i=0;i<19200;++i) image[14+i]=(uint8_t)(i*17);
  RFI_Put32(image+19214,RFI_Sum(image+14,19200));
  memset(&rx,0,sizeof(rx));
  for(unsigned frame=1;frame<=2;++frame) {
    for(unsigned offset=0;offset<sizeof(image);) {
      unsigned take=sizeof(image)-offset;
      if(take>RFI_DATA_MAX) take=RFI_DATA_MAX;
      p=(rfi_packet_t){RFI_DATA,(offset==0?RFI_START:0) |
        (offset+take==sizeof(image)?RFI_END:0),456,frame,offset,(uint16_t)take,image+offset};
      n=RFI_Encode(wire,&p); assert(RFI_Decode(wire,n,&q));
      assert(RFI_Accept(&rx,&q)==1); record(f,wire,n);
      assert(RFI_Accept(&rx,&q)==2);
      offset+=take;
      if(frame==1 && offset>=448) break;
    }
  }
  fclose(f);
  puts("C protocol PASS: maximum packet, ACK loss, duplicate end, gap, reboot, invalid packet, UART fixture");
  return 0;
}
