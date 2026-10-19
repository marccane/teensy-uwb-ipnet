#pragma once

#include <stdint.h>

// DW1000 PHY timing, used for ACK timeouts, TX watchdogs and throughput
// estimates. Figures follow the IEEE 802.15.4a UWB PHY as configured by the
// DW1000 driver (standard SFD at 6.8 Mbps, Decawave SFDs at 850/110 kbps).
namespace phy {

struct Mode {
  uint16_t rateKbps;     // 110, 850 or 6800
  uint8_t prfMhz;        // 16 or 64
  uint16_t preambleLen;  // symbols
};

// SFD length in symbols.
uint8_t sfdSymbols(uint16_t rateKbps);

// Preamble acquisition chunk size the driver selects for a preamble length.
uint8_t pacSize(uint16_t preambleLen);

// On-air duration of a frame with psduLen bytes (FCS included), rounded up.
uint32_t frameAirtimeUs(const Mode& m, uint16_t psduLen);

}  // namespace phy
