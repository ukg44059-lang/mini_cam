#ifndef CC1310_LINK_H
#define CC1310_LINK_H
#include <stdint.h>
void CC_LinkInit(void);
uint32_t CC_LinkWrite(const uint8_t *data, uint32_t len);
void CC_LinkSetSpeed(unsigned level); /* 0=625k, 1=1.25M, 2=2.5M */
#endif
