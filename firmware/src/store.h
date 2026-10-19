#pragma once

#include "config.h"

// Config persistence in the Teensy's emulated EEPROM (1080 bytes on a 4.0).
//
// This firmware writes only from kOffset on, so whatever another firmware
// keeps in the low bytes survives. The emulation spreads every address over
// all 15 flash sectors and rewrites a sector without disturbing other
// addresses, so a separate address range is all the isolation needed.
namespace store {

constexpr int kOffset = 256;

bool load(Config* out);
void save(const Config& cfg);
void erase();

}  // namespace store
