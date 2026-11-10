// Unit tests for the firmware modules that don't touch hardware.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "cobs.h"
#include "config.h"
#include "frame.h"
#include "phy.h"

static int failures = 0;

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                    \
    }                                                                \
  } while (0)

static std::vector<uint8_t> cobsRoundTrip(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> enc(cobs::maxEncodedLen(in.size()));
  const size_t n = cobs::encode(in.data(), in.size(), enc.data());
  CHECK(n <= enc.size());
  for (size_t i = 0; i < n; i++) CHECK(enc[i] != 0);
  std::vector<uint8_t> dec(in.size() + 1);
  size_t outLen = 0;
  CHECK(cobs::decode(enc.data(), n, dec.data(), dec.size(), &outLen));
  dec.resize(outLen);
  // In-place decoding must give the same result.
  size_t inPlaceLen = 0;
  CHECK(cobs::decode(enc.data(), n, enc.data(), enc.size(), &inPlaceLen));
  CHECK(inPlaceLen == outLen);
  CHECK(memcmp(enc.data(), dec.data(), outLen) == 0);
  return dec;
}

static void testCobs() {
  // Known vectors from the COBS paper / Wikipedia.
  {
    const uint8_t in[] = {0x00};
    uint8_t out[4];
    CHECK(cobs::encode(in, 1, out) == 2);
    CHECK(out[0] == 0x01 && out[1] == 0x01);
  }
  {
    const uint8_t in[] = {0x11, 0x22, 0x00, 0x33};
    uint8_t out[8];
    CHECK(cobs::encode(in, 4, out) == 5);
    const uint8_t want[] = {0x03, 0x11, 0x22, 0x02, 0x33};
    CHECK(memcmp(out, want, 5) == 0);
  }
  {
    uint8_t out[4];
    CHECK(cobs::encode(nullptr, 0, out) == 1);
    CHECK(out[0] == 0x01);
  }
  // Round trips around the 254-byte block boundary and at full frame size.
  for (size_t len : {0, 1, 253, 254, 255, 508, 1012, 1100}) {
    std::vector<uint8_t> v(len);
    for (size_t i = 0; i < len; i++) v[i] = (uint8_t)(i * 7 + 1);
    CHECK(cobsRoundTrip(v) == v);
    for (size_t i = 0; i < len; i += 13) v[i] = 0;
    CHECK(cobsRoundTrip(v) == v);
    std::vector<uint8_t> zeros(len, 0);
    CHECK(cobsRoundTrip(zeros) == zeros);
  }
  CHECK(cobs::maxEncodedLen(254) == 256);
  // Malformed input.
  {
    const uint8_t bad1[] = {0x03, 0x11};  // block runs past the end
    uint8_t out[8];
    size_t n;
    CHECK(!cobs::decode(bad1, 2, out, sizeof(out), &n));
    const uint8_t bad2[] = {0x02, 0x00};  // embedded zero
    CHECK(!cobs::decode(bad2, 2, out, sizeof(out), &n));
    const uint8_t ok[] = {0x03, 0x11, 0x22, 0x02, 0x33};
    CHECK(!cobs::decode(ok, 5, out, 3, &n));  // output too small
  }
}

static void testFrame() {
  uint8_t buf[16];
  CHECK(frame::writeDataHeader(buf, 0x42, 0x5557, 0x1234, 0xABCD, true) == frame::kMhrLen);
  CHECK(buf[0] == 0x61 && buf[1] == 0x88);
  frame::Header h;
  CHECK(frame::parse(buf, frame::kMhrLen + 1, &h) == frame::kMhrLen);
  CHECK(h.type == frame::kTypeData && h.ackRequest && h.seq == 0x42);
  CHECK(h.pan == 0x5557 && h.dst == 0x1234 && h.src == 0xABCD);

  frame::writeDataHeader(buf, 7, 1, frame::kBroadcast, 2, false);
  CHECK(buf[0] == 0x41);
  CHECK(frame::parse(buf, frame::kMhrLen, &h) == frame::kMhrLen && !h.ackRequest);

  // The DW1000 auto-ACK: FC 0x0002, seq.
  const uint8_t ack[] = {0x02, 0x00, 0x42};
  CHECK(frame::parse(ack, 3, &h) == frame::kAckLen);
  CHECK(h.type == frame::kTypeAck && h.seq == 0x42);

  // Unsupported: extended addressing, security, truncated.
  frame::writeDataHeader(buf, 1, 1, 2, 3, false);
  buf[1] = 0xCC;
  CHECK(frame::parse(buf, frame::kMhrLen, &h) == 0);
  frame::writeDataHeader(buf, 1, 1, 2, 3, false);
  buf[0] |= 0x08;
  CHECK(frame::parse(buf, frame::kMhrLen, &h) == 0);
  CHECK(frame::parse(buf, 2, &h) == 0);

  CHECK(frame::kMaxPayload == 1011);

  uint8_t ip4[20] = {0x45};
  ip4[9] = 17;
  CHECK(frame::isUdp(ip4, sizeof(ip4)));
  ip4[9] = 6;
  CHECK(!frame::isUdp(ip4, sizeof(ip4)));
  uint8_t ip6[40] = {0x60};
  ip6[6] = 17;
  CHECK(frame::isUdp(ip6, sizeof(ip6)));
  CHECK(!frame::isUdp(ip6, 20));
}

static void testPhy() {
  const phy::Mode fast{6800, 64, 128};
  // 136 preamble+SFD symbols (138.4 us) + PHR (21.5 us) + 9384 data bits (1203.1 us)
  const uint32_t full = phy::frameAirtimeUs(fast, 1023);
  CHECK(full >= 1360 && full <= 1366);
  const uint32_t ack = phy::frameAirtimeUs(fast, 5);
  CHECK(ack >= 168 && ack <= 174);
  const phy::Mode slow{110, 16, 2048};
  const uint32_t slowFull = phy::frameAirtimeUs(slow, 1023);
  CHECK(slowFull > 70000 && slowFull < 80000);
  CHECK(phy::pacSize(128) == 8 && phy::pacSize(256) == 16 && phy::pacSize(1024) == 32 &&
        phy::pacSize(2048) == 64);
}

static void testConfig() {
  Config c = config::defaults();
  CHECK(config::validate(c) == nullptr);
  CHECK(config::effectivePreambleCode(c) == 10);

  uint8_t blob[config::kBlobSize];
  c.peerAddr = 0x1A2B;
  c.txPower = 0x85858585;
  config::serialize(c, blob);
  Config back;
  CHECK(config::deserialize(blob, &back));
  CHECK(memcmp(&back, &c, sizeof(c)) == 0);
  blob[10] ^= 1;
  CHECK(!config::deserialize(blob, &back));
  uint8_t erased[config::kBlobSize];
  memset(erased, 0xFF, sizeof(erased));
  CHECK(!config::deserialize(erased, &back));

  c = config::defaults();
  CHECK(config::set(c, "channel", "2") == nullptr && c.channel == 2);
  CHECK(config::set(c, "peer", "1a2b") == nullptr && c.peerAddr == 0x1A2B);
  CHECK(config::set(c, "pan", "0xBEEF") == nullptr && c.panId == 0xBEEF);
  CHECK(config::set(c, "spi", "12") == nullptr && c.spiHz == 12000000);
  CHECK(config::set(c, "spi", "21") != nullptr);
  CHECK(config::set(c, "ipqueue", "4") == nullptr && c.ipQueue == 4);
  CHECK(config::validate(c) == nullptr);
  c.ipQueue = 0;
  CHECK(config::validate(c) != nullptr);
  c.ipQueue = 17;
  CHECK(config::validate(c) != nullptr);
  c.ipQueue = 4;
  CHECK(config::set(c, "channel", "5x") != nullptr);
  CHECK(config::set(c, "nope", "1") != nullptr);
  CHECK(config::set(c, "profile", "long") == nullptr);
  CHECK(c.rateKbps == 110 && c.preambleLen == 2048);
  CHECK(config::validate(c) == nullptr);

  c = config::defaults();
  c.rateKbps = 110;  // with plen 128
  CHECK(config::validate(c) != nullptr);
  c = config::defaults();
  c.preambleCode = 3;  // 16 MHz code on 64 MHz PRF
  CHECK(config::validate(c) != nullptr);
  c.prfMhz = 16;
  CHECK(config::validate(c) == nullptr);
  c = config::defaults();
  c.shortAddr = 0x10;
  c.peerAddr = 0x10;
  CHECK(config::validate(c) != nullptr);

  // CRC-32 check value.
  CHECK(config::crc32((const uint8_t*)"123456789", 9) == 0xCBF43926);
}

int main() {
  testCobs();
  testFrame();
  testPhy();
  testConfig();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
