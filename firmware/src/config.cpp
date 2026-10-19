#include "config.h"

#include <stdlib.h>
#include <string.h>

namespace config {

namespace {

constexpr uint8_t kMagic[4] = {'U', 'W', 'B', 'N'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderLen = 8;
constexpr size_t kPayloadLen = kBlobSize - kHeaderLen - 4;

constexpr uint32_t kMaxSpiHz = 20000000;  // DW1000 limit with the PLL running
constexpr uint32_t kMinSpiHz = 1000000;

bool parseUint(const char* s, int base, uint32_t max, uint32_t* out) {
  if (!s || !*s) return false;
  char* end;
  const unsigned long v = strtoul(s, &end, base);
  if (*end != '\0' || v > max) return false;
  *out = (uint32_t)v;
  return true;
}

bool validPreambleLen(uint16_t len) {
  switch (len) {
    case 64:
    case 128:
    case 256:
    case 512:
    case 1024:
    case 1536:
    case 2048:
    case 4096:
      return true;
    default:
      return false;
  }
}

// DW1000 user manual, table 61.
bool validPreambleCode(uint8_t channel, uint8_t prfMhz, uint8_t code) {
  if (prfMhz == 64) {
    if (channel == 4 || channel == 7) return code >= 17 && code <= 20;
    return code >= 9 && code <= 12;
  }
  switch (channel) {
    case 1:
      return code == 1 || code == 2;
    case 2:
    case 5:
      return code == 3 || code == 4;
    case 3:
      return code == 5 || code == 6;
    default:  // 4, 7
      return code == 7 || code == 8;
  }
}

class Writer {
 public:
  explicit Writer(uint8_t* p) : p_(p) {}
  void u8(uint8_t v) { *p_++ = v; }
  void u16(uint16_t v) {
    u8(v & 0xFF);
    u8(v >> 8);
  }
  void u32(uint32_t v) {
    u16(v & 0xFFFF);
    u16(v >> 16);
  }

 private:
  uint8_t* p_;
};

class Reader {
 public:
  explicit Reader(const uint8_t* p) : p_(p) {}
  uint8_t u8() { return *p_++; }
  uint16_t u16() {
    const uint16_t lo = u8();
    return lo | (u8() << 8);
  }
  uint32_t u32() {
    const uint32_t lo = u16();
    return lo | ((uint32_t)u16() << 16);
  }

 private:
  const uint8_t* p_;
};

}  // namespace

const char* const kKeysHelp =
    "  channel 1|2|3|4|5|7      UWB channel (both ends)\n"
    "  prf 16|64                pulse repetition frequency, MHz (both ends)\n"
    "  rate 110|850|6800        data rate, kbit/s (both ends)\n"
    "  plen 64..4096            preamble length, symbols (both ends)\n"
    "  pcode 0|1..24            preamble code, 0 = default for channel (both ends)\n"
    "  profile fast|mid|long    6800k/128, 850k/512 or 110k/2048 (both ends)\n"
    "  pan <hex>                PAN ID (both ends)\n"
    "  addr <hex>               own short address, 0 = from Teensy MAC\n"
    "  peer <hex>               peer address, 0 = learn, ffff = broadcast only\n"
    "  retries 0..15            link retransmissions, 0 = no ACKs\n"
    "  udparq 0|1               also ACK/retransmit UDP packets\n"
    "  backoff 0..10000         pause after own frame so the peer can reply, us\n"
    "  txpower <hex>            TX_POWER register, 0 = reference value\n"
    "  spi 1..20                SPI clock, MHz\n";

Config defaults() {
  Config c;
  c.channel = 5;
  c.prfMhz = 64;
  c.rateKbps = 6800;
  c.preambleLen = 128;
  c.preambleCode = 0;
  c.retries = 3;
  c.udpArq = 1;
  c.txPower = 0;
  c.panId = 0x5557;  // "UW"
  c.shortAddr = 0;
  c.peerAddr = 0;
  c.backoffUs = 300;
  c.spiHz = kMaxSpiHz;
  return c;
}

uint8_t defaultPreambleCode(uint8_t channel, uint8_t prfMhz) {
  const bool p16 = prfMhz == 16;
  switch (channel) {
    case 1:
      return p16 ? 2 : 10;
    case 3:
      return p16 ? 6 : 10;
    case 4:
    case 7:
      return p16 ? 8 : 18;
    default:  // 2, 5
      return p16 ? 4 : 10;
  }
}

uint8_t effectivePreambleCode(const Config& cfg) {
  return cfg.preambleCode ? cfg.preambleCode : defaultPreambleCode(cfg.channel, cfg.prfMhz);
}

const char* validate(const Config& c) {
  switch (c.channel) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 7:
      break;
    default:
      return "channel must be 1, 2, 3, 4, 5 or 7";
  }
  if (c.prfMhz != 16 && c.prfMhz != 64) return "prf must be 16 or 64";
  if (c.rateKbps != 110 && c.rateKbps != 850 && c.rateKbps != 6800)
    return "rate must be 110, 850 or 6800";
  if (!validPreambleLen(c.preambleLen))
    return "plen must be 64, 128, 256, 512, 1024, 1536, 2048 or 4096";
  // The driver only has receiver tuning for these combinations (DRX_TUNE1b).
  if (c.rateKbps == 110 && c.preambleLen < 1536) return "rate 110 needs plen >= 1536";
  if (c.rateKbps != 110 && c.preambleLen >= 1536) return "plen >= 1536 needs rate 110";
  if (c.preambleLen == 64 && c.rateKbps != 6800) return "plen 64 needs rate 6800";
  if (c.preambleCode && !validPreambleCode(c.channel, c.prfMhz, c.preambleCode))
    return "pcode not valid for this channel and prf";
  if (c.retries > 15) return "retries must be 0..15";
  if (c.udpArq > 1) return "udparq must be 0 or 1";
  if (c.panId == 0xFFFF) return "pan ffff is the broadcast PAN";
  if (c.shortAddr >= 0xFFFE) return "addr fffe/ffff are reserved";
  if (c.peerAddr == 0xFFFE) return "peer fffe is reserved";
  if (c.shortAddr && c.peerAddr == c.shortAddr) return "peer equals own addr";
  if (c.backoffUs > 10000) return "backoff must be 0..10000";
  if (c.spiHz < kMinSpiHz || c.spiHz > kMaxSpiHz) return "spi must be 1..20 MHz";
  return nullptr;
}

const char* set(Config& c, const char* key, const char* value) {
  uint32_t v;
  if (!strcmp(key, "profile")) {
    if (!strcmp(value, "fast")) {
      c.rateKbps = 6800;
      c.preambleLen = 128;
    } else if (!strcmp(value, "mid")) {
      c.rateKbps = 850;
      c.preambleLen = 512;
    } else if (!strcmp(value, "long")) {
      c.rateKbps = 110;
      c.preambleLen = 2048;
    } else {
      return "profile must be fast, mid or long";
    }
    return nullptr;
  }

  struct Field {
    const char* key;
    int base;
    uint32_t max;
  };
  static const Field kFields[] = {
      {"channel", 10, 255}, {"prf", 10, 255},     {"rate", 10, 65535},  {"plen", 10, 65535},
      {"pcode", 10, 255},   {"retries", 10, 255}, {"udparq", 10, 255},  {"txpower", 16, 0xFFFFFFFF},
      {"pan", 16, 0xFFFF},  {"addr", 16, 0xFFFF}, {"peer", 16, 0xFFFF}, {"backoff", 10, 65535},
      {"spi", 10, 20},
  };
  const Field* f = nullptr;
  for (const Field& candidate : kFields) {
    if (!strcmp(key, candidate.key)) f = &candidate;
  }
  if (!f) return "unknown key";
  if (!parseUint(value, f->base, f->max, &v)) return "bad value";

  if (!strcmp(key, "channel")) c.channel = v;
  else if (!strcmp(key, "prf")) c.prfMhz = v;
  else if (!strcmp(key, "rate")) c.rateKbps = v;
  else if (!strcmp(key, "plen")) c.preambleLen = v;
  else if (!strcmp(key, "pcode")) c.preambleCode = v;
  else if (!strcmp(key, "retries")) c.retries = v;
  else if (!strcmp(key, "udparq")) c.udpArq = v;
  else if (!strcmp(key, "txpower")) c.txPower = v;
  else if (!strcmp(key, "pan")) c.panId = v;
  else if (!strcmp(key, "addr")) c.shortAddr = v;
  else if (!strcmp(key, "peer")) c.peerAddr = v;
  else if (!strcmp(key, "backoff")) c.backoffUs = v;
  else if (!strcmp(key, "spi")) c.spiHz = v * 1000000;
  return nullptr;
}

void serialize(const Config& c, uint8_t* blob) {
  memcpy(blob, kMagic, sizeof(kMagic));
  Writer w(blob + sizeof(kMagic));
  w.u16(kVersion);
  w.u16(kPayloadLen);
  w.u8(c.channel);
  w.u8(c.prfMhz);
  w.u16(c.rateKbps);
  w.u16(c.preambleLen);
  w.u8(c.preambleCode);
  w.u8(c.retries);
  w.u8(c.udpArq);
  w.u32(c.txPower);
  w.u16(c.panId);
  w.u16(c.shortAddr);
  w.u16(c.peerAddr);
  w.u16(c.backoffUs);
  w.u32(c.spiHz);
  Writer(blob + kHeaderLen + kPayloadLen).u32(crc32(blob, kHeaderLen + kPayloadLen));
}

bool deserialize(const uint8_t* blob, Config* out) {
  if (memcmp(blob, kMagic, sizeof(kMagic)) != 0) return false;
  Reader r(blob + sizeof(kMagic));
  if (r.u16() != kVersion || r.u16() != kPayloadLen) return false;
  if (Reader(blob + kHeaderLen + kPayloadLen).u32() != crc32(blob, kHeaderLen + kPayloadLen))
    return false;
  Config c;
  c.channel = r.u8();
  c.prfMhz = r.u8();
  c.rateKbps = r.u16();
  c.preambleLen = r.u16();
  c.preambleCode = r.u8();
  c.retries = r.u8();
  c.udpArq = r.u8();
  c.txPower = r.u32();
  c.panId = r.u16();
  c.shortAddr = r.u16();
  c.peerAddr = r.u16();
  c.backoffUs = r.u16();
  c.spiHz = r.u32();
  if (validate(c)) return false;
  *out = c;
  return true;
}

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

}  // namespace config
