#include "cobs.h"

namespace cobs {

size_t encode(const uint8_t* in, size_t len, uint8_t* out) {
  size_t write = 1;
  size_t codeIdx = 0;
  uint8_t code = 1;
  for (size_t read = 0; read < len; read++) {
    if (in[read] == 0) {
      out[codeIdx] = code;
      code = 1;
      codeIdx = write++;
    } else {
      out[write++] = in[read];
      if (++code == 0xFF) {
        out[codeIdx] = code;
        code = 1;
        codeIdx = write++;
      }
    }
  }
  out[codeIdx] = code;
  return write;
}

bool decode(const uint8_t* in, size_t len, uint8_t* out, size_t outCap, size_t* outLen) {
  size_t read = 0;
  size_t write = 0;
  while (read < len) {
    const uint8_t code = in[read++];
    if (code == 0) return false;
    for (uint8_t i = 1; i < code; i++) {
      if (read >= len || write >= outCap || in[read] == 0) return false;
      out[write++] = in[read++];
    }
    // A block shorter than 254 data bytes stands for a zero, except at the end.
    if (code != 0xFF && read < len) {
      if (write >= outCap) return false;
      out[write++] = 0;
    }
  }
  *outLen = write;
  return true;
}

}  // namespace cobs
