#pragma once

#include <stddef.h>
#include <stdint.h>

// Link and radio settings. Both boards must agree on channel, PRF, rate,
// preamble length/code and PAN; the rest is per board.
struct Config {
  uint8_t channel;       // 1, 2, 3, 4, 5 or 7
  uint8_t prfMhz;        // 16 or 64
  uint16_t rateKbps;     // 110, 850 or 6800
  uint16_t preambleLen;  // 64 .. 4096 symbols
  uint8_t preambleCode;  // 0 = default for channel and PRF
  uint8_t retries;       // link-layer retransmissions; 0 = never request ACKs
  uint8_t udpArq;        // 0 = send UDP without ACK/retransmission
  uint32_t txPower;      // TX_POWER register value; 0 = DW1000 reference value
  uint16_t panId;
  uint16_t shortAddr;    // 0 = derived from the Teensy's factory MAC
  uint16_t peerAddr;     // 0 = learn from the first frame heard, 0xFFFF = always broadcast
  uint16_t backoffUs;    // pause after our own frame before the next, so the peer can reply
  uint32_t spiHz;        // DW1000 SPI clock once the chip runs from its PLL
};

namespace config {

Config defaults();

// nullptr if cfg is usable, otherwise what is wrong with it.
const char* validate(const Config& cfg);

// Applies one "key value" setting, e.g. ("channel", "5"). Returns nullptr on
// success or an error message. Does not check combinations; call validate().
const char* set(Config& cfg, const char* key, const char* value);

// Help text listing the keys accepted by set().
extern const char* const kKeysHelp;

// Preamble code the driver picks for a channel/PRF pair.
uint8_t defaultPreambleCode(uint8_t channel, uint8_t prfMhz);
uint8_t effectivePreambleCode(const Config& cfg);

// Persistent form: magic, version, fields, CRC-32.
constexpr size_t kBlobSize = 37;
void serialize(const Config& cfg, uint8_t* blob);
bool deserialize(const uint8_t* blob, Config* out);

uint32_t crc32(const uint8_t* data, size_t len);

}  // namespace config
