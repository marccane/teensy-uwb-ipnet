#pragma once

#include <stdint.h>

// Wiring of the DW1000. It sits on SPI0: SCK 13, MOSI 11, MISO 12. The
// firmware uses no other pins.
namespace board {

constexpr uint8_t kDwCs = 10;
constexpr uint8_t kDwRst = 9;
constexpr uint8_t kDwIrq = 3;

}  // namespace board
