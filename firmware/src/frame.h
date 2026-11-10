#pragma once

#include <stddef.h>
#include <stdint.h>

// Over-the-air frame format.
//
// Every frame is an IEEE 802.15.4 frame so the DW1000 can filter by PAN and
// address and send ACKs in hardware:
//
//   data: FC(2) seq(1) dstPAN(2) dst(2) src(2) | dispatch(1) payload | FCS(2)
//   ack:  FC(2) seq(1)                                                | FCS(2)
//
// The DW1000's extended PHR allows PSDUs of up to 1023 bytes (both ends must
// run in that mode), which leaves room for a 1011-byte IP packet.
namespace frame {

constexpr uint16_t kBroadcast = 0xFFFF;

constexpr size_t kMhrLen = 9;
constexpr size_t kFcsLen = 2;
constexpr size_t kAckLen = 3;  // without FCS
constexpr size_t kMaxPsdu = 1023;
constexpr size_t kMaxMsdu = kMaxPsdu - kMhrLen - kFcsLen;  // dispatch + payload
constexpr size_t kMaxPayload = kMaxMsdu - 1;

// First MSDU byte: what the rest of the frame carries.
enum Dispatch : uint8_t {
  kIp = 0x01,     // one IPv4/IPv6 packet, forwarded to the host
  kHello = 0x02,  // broadcast presence beacon (peer discovery, link status)
  kTest = 0x03,   // link test traffic (console ping/flood)
  kEcho = 0x04,   // reply to a kTest frame that asked for one
};

// IEEE 802.15.4 frame types.
constexpr uint8_t kTypeData = 1;
constexpr uint8_t kTypeAck = 2;

// Frame control, first byte.
constexpr uint8_t kFcFramePending = 0x10;
constexpr uint8_t kFcAckRequest = 0x20;

struct Header {
  uint8_t type;
  bool framePending;  // in an ACK: the peer has frames waiting to send
  bool ackRequest;
  uint8_t seq;
  uint16_t pan;  // data frames only
  uint16_t dst;
  uint16_t src;
};

// Writes the 9-byte data frame MAC header. Returns kMhrLen.
size_t writeDataHeader(uint8_t* out, uint8_t seq, uint16_t pan, uint16_t dst, uint16_t src,
                       bool ackRequest);

// Parses a received PSDU (FCS already stripped). Accepts ACKs and data frames
// with PAN ID compression and short addresses, which is all this firmware
// sends. Returns the header length, or 0 for anything else.
size_t parse(const uint8_t* psdu, size_t len, Header* h);

// True if an IPv4/IPv6 packet carries UDP directly (no IPv6 extension headers).
bool isUdp(const uint8_t* ip, size_t len);

}  // namespace frame
