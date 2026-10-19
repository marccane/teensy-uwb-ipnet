#pragma once

#include <stddef.h>
#include <stdint.h>

// Radio link tests driven from the console. They talk to the peer's firmware
// directly (kTest/kEcho frames), without involving either host.
namespace linktest {

// Sends count echo requests of size bytes, one at a time, and reports RTTs.
void startPing(uint16_t count, uint16_t size);

// Keeps the radio queue full of test frames for durationMs and reports the
// delivered throughput. The peer logs what it received.
void startFlood(uint32_t durationMs, uint16_t size);

void stop();
bool busy();
void poll();

// kTest / kEcho frames from the radio.
void onFrame(uint16_t src, const uint8_t* msdu, size_t len);

}  // namespace linktest
