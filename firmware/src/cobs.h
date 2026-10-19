#pragma once

#include <stddef.h>
#include <stdint.h>

// Consistent Overhead Byte Stuffing. The encoded form never contains 0x00, so
// a single 0x00 byte delimits frames on the USB byte stream.
namespace cobs {

// Worst-case encoded size of n input bytes, not counting the delimiter.
constexpr size_t maxEncodedLen(size_t n) { return n + n / 254 + 1; }

// Encodes len bytes from in. out must hold maxEncodedLen(len) bytes.
// Returns the encoded length.
size_t encode(const uint8_t* in, size_t len, uint8_t* out);

// Decodes len bytes (without the delimiter). out may alias in. Returns false
// if the input is malformed or the result would exceed outCap.
bool decode(const uint8_t* in, size_t len, uint8_t* out, size_t outCap, size_t* outLen);

}  // namespace cobs
