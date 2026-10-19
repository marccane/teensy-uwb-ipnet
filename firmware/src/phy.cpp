#include "phy.h"

namespace phy {

namespace {

// Durations in picoseconds.
constexpr uint64_t kPreambleSymbolPs16 = 993590;   // 16 MHz PRF
constexpr uint64_t kPreambleSymbolPs64 = 1017630;  // 64 MHz PRF
constexpr uint64_t kBitPs110k = 8205130;
constexpr uint64_t kBitPs850k = 1025640;
constexpr uint64_t kBitPs6m8 = 128210;

// PHR is 19 bits; rounded up to 21 to stay conservative. It is sent at
// 850 kbps for the 850k and 6.8M rates, and at 110 kbps otherwise.
constexpr uint32_t kPhrBits = 21;

// Reed-Solomon: 48 parity bits for every (started) block of 330 data bits.
constexpr uint32_t kRsBlockBits = 330;
constexpr uint32_t kRsParityBits = 48;

uint64_t bitPs(uint16_t rateKbps) {
  switch (rateKbps) {
    case 110:
      return kBitPs110k;
    case 850:
      return kBitPs850k;
    default:
      return kBitPs6m8;
  }
}

}  // namespace

uint8_t sfdSymbols(uint16_t rateKbps) {
  switch (rateKbps) {
    case 110:
      return 64;
    case 850:
      return 16;
    default:
      return 8;
  }
}

uint8_t pacSize(uint16_t preambleLen) {
  if (preambleLen <= 128) return 8;
  if (preambleLen <= 512) return 16;
  if (preambleLen <= 1024) return 32;
  return 64;
}

uint32_t frameAirtimeUs(const Mode& m, uint16_t psduLen) {
  const uint64_t symPs = m.prfMhz == 16 ? kPreambleSymbolPs16 : kPreambleSymbolPs64;
  const uint64_t shrPs = (uint64_t)(m.preambleLen + sfdSymbols(m.rateKbps)) * symPs;
  const uint64_t phrPs = kPhrBits * (m.rateKbps == 110 ? kBitPs110k : kBitPs850k);
  const uint32_t dataBits = 8u * psduLen;
  const uint32_t blocks = (dataBits + kRsBlockBits - 1) / kRsBlockBits;
  const uint64_t dataPs = (uint64_t)(dataBits + blocks * kRsParityBits) * bitPs(m.rateKbps);
  return (uint32_t)((shrPs + phrPs + dataPs + 999999) / 1000000);
}

}  // namespace phy
