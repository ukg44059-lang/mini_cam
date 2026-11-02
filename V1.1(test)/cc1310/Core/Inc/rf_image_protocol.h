#ifndef RF_IMAGE_PROTOCOL_H
#define RF_IMAGE_PROTOCOL_H
#include <stdint.h>
#include <stdbool.h>

/* All integers little endian. RF CRC protects each packet. UART adds a sum32.
 * RI, version, type, session32, frame32, offset32, length16, flags, reserved. */
#define RFI_HEADER 20U
#define RFI_DATA_MAX 224U
#define RFI_PACKET_MAX (RFI_HEADER + RFI_DATA_MAX)
#define RFI_DATA 1U
#define RFI_ACK 2U
#define RFI_START 1U
#define RFI_END 2U
typedef struct {
  uint8_t type, flags;
  uint32_t session, frame, offset;
  uint16_t length;
  const uint8_t *data;
} rfi_packet_t;
typedef struct {
  bool active, have_last;
  uint32_t session, frame, next, last_offset, last_hash;
  uint16_t last_length;
  uint8_t last_flags;
} rfi_receiver_t;
uint32_t RFI_Get32(const uint8_t *p);
void RFI_Put32(uint8_t *p, uint32_t v);
uint32_t RFI_Sum(const uint8_t *p, unsigned n);
unsigned RFI_Encode(uint8_t *out, const rfi_packet_t *p);
bool RFI_Decode(const uint8_t *data, unsigned size, rfi_packet_t *p);
/* 1 = new bytes, 2 = duplicate (ACK again without writing UART), 0 = reject. */
int RFI_Accept(rfi_receiver_t *s, const rfi_packet_t *p);
#endif
