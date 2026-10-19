#include "store.h"

#include <EEPROM.h>

namespace store {

static_assert(kOffset + config::kBlobSize <= E2END + 1, "config does not fit the EEPROM");

bool load(Config* out) {
  uint8_t blob[config::kBlobSize];
  for (size_t i = 0; i < sizeof(blob); i++) blob[i] = EEPROM.read(kOffset + i);
  return config::deserialize(blob, out);
}

void save(const Config& cfg) {
  uint8_t blob[config::kBlobSize];
  config::serialize(cfg, blob);
  // update() skips bytes that already hold the value.
  for (size_t i = 0; i < sizeof(blob); i++) EEPROM.update(kOffset + i, blob[i]);
}

void erase() {
  for (size_t i = 0; i < config::kBlobSize; i++) EEPROM.update(kOffset + i, 0xFF);
}

}  // namespace store
