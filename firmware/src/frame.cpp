#include "frame.h"

namespace frame {

namespace {

// Frame control for data frames: type data, PAN ID compression, short
// destination and source addresses, frame version 0 (802.15.4-2003), which
// the DW1000 frame filter accepts.
constexpr uint8_t kFcDataLow = 0x41;
constexpr uint8_t kFcDataHigh = 0x88;

constexpr uint8_t kFcTypeMask = 0x07;
constexpr uint8_t kFcSecurity = 0x08;
constexpr uint8_t kFcPanCompression = 0x40;

void put16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}

uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }

}  // namespace

size_t writeDataHeader(uint8_t* out, uint8_t seq, uint16_t pan, uint16_t dst, uint16_t src,
                       bool ackRequest) {
  out[0] = kFcDataLow | (ackRequest ? kFcAckRequest : 0);
  out[1] = kFcDataHigh;
  out[2] = seq;
  put16(out + 3, pan);
  put16(out + 5, dst);
  put16(out + 7, src);
  return kMhrLen;
}

size_t parse(const uint8_t* psdu, size_t len, Header* h) {
  if (len < kAckLen) return 0;
  const uint8_t type = psdu[0] & kFcTypeMask;
  h->type = type;
  h->ackRequest = psdu[0] & kFcAckRequest;
  h->seq = psdu[2];
  if (type == kTypeAck) {
    h->pan = h->dst = h->src = 0;
    return kAckLen;
  }
  if (type != kTypeData || len < kMhrLen) return 0;
  if ((psdu[0] & (kFcSecurity | kFcPanCompression)) != kFcPanCompression) return 0;
  // Both address modes must be "short"; ignore the frame version bits.
  if ((psdu[1] & 0xCC) != (kFcDataHigh & 0xCC)) return 0;
  h->pan = get16(psdu + 3);
  h->dst = get16(psdu + 5);
  h->src = get16(psdu + 7);
  return kMhrLen;
}

bool isUdp(const uint8_t* ip, size_t len) {
  constexpr uint8_t kProtoUdp = 17;
  if (len < 1) return false;
  switch (ip[0] >> 4) {
    case 4:
      return len >= 20 && ip[9] == kProtoUdp;
    case 6:
      return len >= 40 && ip[6] == kProtoUdp;
    default:
      return false;
  }
}

}  // namespace frame
