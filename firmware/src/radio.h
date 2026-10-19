#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// DW1000 data link: frame queue, channel access, hardware ACKs with
// retransmission, duplicate suppression and peer discovery.
namespace radio {

// Called for each data frame from the peer that is not a duplicate and not a
// HELLO beacon. msdu[0] is the dispatch byte (see frame.h).
using RxHandler = void (*)(uint16_t src, const uint8_t* msdu, size_t len);

constexpr size_t kTxQueueLen = 16;

// One-time pin/SPI setup, then reconfigure(). False if no DW1000 answers.
bool begin(const Config& cfg);

// Resets the DW1000 and applies cfg. Queued frames are kept. Falls back to a
// slower SPI clock if the configured one fails the integrity check.
bool reconfigure(const Config& cfg);

void setRxHandler(RxHandler handler);

// Services DW1000 events, timeouts, beacons and the transmit queue.
void poll();

// Queues a frame; msdu[0] is the dispatch byte. dst 0 sends to the peer
// (broadcast while none is known). arq=false never requests an ACK.
bool send(const uint8_t* msdu, size_t len, uint16_t dst = 0, bool arq = true);
size_t txQueueFree();
bool txIdle();

bool ready();
uint32_t deviceId();
uint16_t address();
uint16_t peer();    // 0 while unknown
bool linkUp();      // peer heard in the last few seconds
uint32_t spiClock();
uint32_t ackTimeoutUs();
uint32_t maxFrameAirtimeUs();

// SPI integrity test at hz: DEV_ID reads, write/read-back of a 41-byte
// register and repeated 1 KB buffer reads. Returns the number of failed
// checks. Leaves the radio stopped; call reconfigure() afterwards.
uint32_t spiTest(uint32_t hz, uint32_t rounds, uint32_t* bulkReadUs);

// Debug access to any register. Stops nothing; use with care while running.
void readRegister(uint8_t reg, uint16_t sub, uint8_t* out, uint16_t len);

}  // namespace radio
