#include "wireless_control.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
  uint8_t p[255],data[224]={0};unsigned n;
  assert(WC_Check((const uint8_t *)"hello",5)==0x4f9f2cabU);
  n=WC_Encode(p,WC_POLL,123,0,data,8);assert(WC_Valid(p,n));
  assert(!WC_Valid(p,n-1));p[14]=1;assert(!WC_Valid(p,n));
  n=WC_Encode(p,WC_REPLY,456,7,data,160);assert(WC_Valid(p,n));
  assert(WC_Get32(p+4)==456 && WC_Get32(p+8)==7);
  n=WC_Encode(p,WC_REPLY,456,7,data,161);assert(!WC_Valid(p,n));
  n=WC_Encode(p,WC_REPLY,456,0,0,0);assert(WC_Valid(p,n));
  n=WC_Encode(p,WC_STATUS,123,8,data,224);assert(n==240 && WC_Valid(p,n));
  n=WC_Encode(p,WC_STATUS,123,8,0,0);assert(!WC_Valid(p,n));
  n=WC_Encode(p,WC_ACK,123,8,0,0);assert(WC_Valid(p,n));
  p[3]=99;assert(!WC_Valid(p,n));p[3]=WC_ACK;p[2]=2;assert(!WC_Valid(p,n));
  puts("Wireless C protocol PASS: bounds, types, version, endianness, FNV-1a");
  return 0;
}
