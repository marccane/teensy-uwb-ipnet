#include "linktest.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "frame.h"
#include "log.h"
#include "radio.h"
#include "stats.h"

namespace linktest {

namespace {

// Test frame: dispatch(1) flags(1) id(2) seq(2) sentUs(4) padding...
constexpr uint8_t kFlagEchoRequest = 0x01;
constexpr size_t kMinSize = 10;
constexpr uint32_t kPingTimeoutMs = 1000;
constexpr uint32_t kPingGapMs = 100;
constexpr uint32_t kFloodDrainMs = 3000;
constexpr uint32_t kRxReportIdleMs = 1000;

enum class Mode { kIdle, kPing, kFlood, kFloodDrain };

Mode mode = Mode::kIdle;
uint16_t testId = 0;
uint16_t frameSize = 0;
uint8_t buf[frame::kMaxMsdu];

uint16_t pingCount = 0;
uint16_t pingSeq = 0;
uint16_t pingReceived = 0;
bool pingWaiting = false;
uint32_t pingSentMs = 0;
uint32_t pingNextMs = 0;
uint32_t rttMin = 0;
uint32_t rttMax = 0;
uint64_t rttSum = 0;

uint32_t floodStartMs = 0;
uint32_t floodEndMs = 0;
uint32_t floodDrainStartMs = 0;
uint16_t floodSeq = 0;
Stats floodBase;

// Receiving side of a flood: one report per burst.
bool burstActive = false;
uint16_t burstSrc = 0;
uint16_t burstId = 0;
uint16_t burstNextSeq = 0;
uint32_t burstFrames = 0;
uint32_t burstBytes = 0;
uint32_t burstLost = 0;
uint32_t burstStartMs = 0;
uint32_t burstLastMs = 0;

void put16(uint8_t* p, uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}

void put32(uint8_t* p, uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}

uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t get32(const uint8_t* p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

int dbm(float v) { return isfinite(v) ? (int)lroundf(v) : -999; }

size_t buildTest(uint8_t flags, uint16_t seq) {
  buf[0] = frame::kTest;
  buf[1] = flags;
  put16(buf + 2, testId);
  put16(buf + 4, seq);
  put32(buf + 6, micros());
  for (size_t i = kMinSize; i < frameSize; i++) buf[i] = (uint8_t)i;
  return frameSize;
}

uint16_t clampSize(uint16_t size) {
  if (size < kMinSize) return kMinSize;
  if (size > frame::kMaxMsdu) return frame::kMaxMsdu;
  return size;
}

void finishPing() {
  const uint16_t lost = pingCount - pingReceived;
  if (pingReceived) {
    logPrintf("ping: %u sent, %u received, %u%% loss, rtt min/avg/max %lu/%lu/%lu us", pingCount,
              pingReceived, 100u * lost / pingCount, rttMin, (uint32_t)(rttSum / pingReceived),
              rttMax);
  } else {
    logPrintf("ping: %u sent, 0 received", pingCount);
  }
  mode = Mode::kIdle;
}

void pollPing(uint32_t ms) {
  if (pingWaiting && ms - pingSentMs > kPingTimeoutMs) {
    logPrintf("seq %u: no reply", pingSeq);
    pingWaiting = false;
    pingSeq++;
    pingNextMs = ms + kPingGapMs;
  }
  if (pingWaiting) return;
  if (pingSeq >= pingCount) {
    finishPing();
    return;
  }
  if ((int32_t)(ms - pingNextMs) >= 0 && radio::txQueueFree() > 0) {
    radio::send(buf, buildTest(kFlagEchoRequest, pingSeq));
    pingWaiting = true;
    pingSentMs = ms;
  }
}

void finishFlood(uint32_t ms) {
  const uint32_t elapsed = ms - floodStartMs;
  const uint32_t acked = stats.txAcked - floodBase.txAcked;
  const uint32_t noAck = stats.txNoAck - floodBase.txNoAck;
  const uint32_t failed = stats.txFailed - floodBase.txFailed;
  const uint32_t attempts = stats.txAttempts - floodBase.txAttempts;
  const uint32_t frames = acked + noAck + failed;
  const uint32_t bytes = stats.txBytes - floodBase.txBytes;
  logPrintf("flood: %lu frames in %lu ms: %lu acked, %lu unacked, %lu failed, %lu retransmissions",
            frames, elapsed, acked, noAck, failed, attempts > frames ? attempts - frames : 0);
  logPrintf("flood: %lu kbit/s delivered (MSDU bytes), %lu channel defers",
            elapsed ? (uint32_t)((uint64_t)bytes * 8 / elapsed) : 0,
            stats.ccaDefers - floodBase.ccaDefers);
  mode = Mode::kIdle;
}

void pollFlood(uint32_t ms) {
  if (mode == Mode::kFlood) {
    // Keep one slot free so beacons and echo replies still get through.
    while ((int32_t)(ms - floodEndMs) < 0 && radio::txQueueFree() > 1) {
      radio::send(buf, buildTest(0, floodSeq++));
    }
    if ((int32_t)(ms - floodEndMs) >= 0) {
      mode = Mode::kFloodDrain;
      floodDrainStartMs = ms;
    }
    return;
  }
  if (radio::txIdle() || ms - floodDrainStartMs > kFloodDrainMs) finishFlood(ms);
}

void pollBurstReport(uint32_t ms) {
  if (!burstActive || ms - burstLastMs <= kRxReportIdleMs) return;
  const uint32_t elapsed = burstLastMs - burstStartMs;
  logPrintf("test rx from %04x: %lu frames, %lu lost, %lu kbit/s over %lu ms, rssi %d dBm",
            burstSrc, burstFrames, burstLost,
            elapsed ? (uint32_t)((uint64_t)burstBytes * 8 / elapsed) : 0, elapsed,
            dbm(stats.rssiDbm));
  burstActive = false;
}

void onTestFrame(uint16_t src, const uint8_t* msdu, size_t len) {
  if (msdu[1] & kFlagEchoRequest) {
    memcpy(buf, msdu, len);
    buf[0] = frame::kEcho;
    radio::send(buf, len, src);
    return;
  }
  stats.testRxFrames++;
  stats.testRxBytes += len;
  const uint32_t ms = millis();
  const uint16_t id = get16(msdu + 2);
  const uint16_t seq = get16(msdu + 4);
  if (!burstActive || id != burstId || src != burstSrc) {
    burstActive = true;
    burstSrc = src;
    burstId = id;
    burstNextSeq = seq;
    burstFrames = burstBytes = burstLost = 0;
    burstStartMs = ms;
  }
  burstLost += (uint16_t)(seq - burstNextSeq);
  burstNextSeq = seq + 1;
  burstFrames++;
  burstBytes += len;
  burstLastMs = ms;
}

void onEchoFrame(uint16_t src, const uint8_t* msdu, size_t len) {
  if (mode != Mode::kPing || !pingWaiting) return;
  if (get16(msdu + 2) != testId || get16(msdu + 4) != pingSeq) return;
  const uint32_t rtt = micros() - get32(msdu + 6);
  logPrintf("%u bytes from %04x: seq=%u rtt=%lu us rssi=%d dBm", (unsigned)len, src, pingSeq, rtt,
            dbm(stats.rssiDbm));
  pingReceived++;
  rttSum += rtt;
  if (rtt < rttMin) rttMin = rtt;
  if (rtt > rttMax) rttMax = rtt;
  pingWaiting = false;
  pingSeq++;
  pingNextMs = millis() + kPingGapMs;
}

}  // namespace

void startPing(uint16_t count, uint16_t size) {
  stop();
  frameSize = clampSize(size);
  testId = micros();
  pingCount = count ? count : 1;
  pingSeq = pingReceived = 0;
  pingWaiting = false;
  pingNextMs = millis();
  rttMin = UINT32_MAX;
  rttMax = 0;
  rttSum = 0;
  mode = Mode::kPing;
  logPrintf("ping %04x: %u frames of %u bytes", radio::peer(), pingCount, frameSize);
}

void startFlood(uint32_t durationMs, uint16_t size) {
  stop();
  frameSize = clampSize(size);
  testId = micros();
  floodSeq = 0;
  floodBase = stats;
  floodStartMs = millis();
  floodEndMs = floodStartMs + durationMs;
  mode = Mode::kFlood;
  logPrintf("flood %04x: %lu ms of %u-byte frames", radio::peer(), durationMs, frameSize);
}

void stop() {
  if (mode == Mode::kIdle) return;
  mode = Mode::kIdle;
  logPrintf("test stopped");
}

bool busy() { return mode != Mode::kIdle; }

void poll() {
  const uint32_t ms = millis();
  switch (mode) {
    case Mode::kPing:
      pollPing(ms);
      break;
    case Mode::kFlood:
    case Mode::kFloodDrain:
      pollFlood(ms);
      break;
    case Mode::kIdle:
      break;
  }
  pollBurstReport(ms);
}

void onFrame(uint16_t src, const uint8_t* msdu, size_t len) {
  if (len < kMinSize) return;
  if (msdu[0] == frame::kTest) {
    onTestFrame(src, msdu, len);
  } else if (msdu[0] == frame::kEcho) {
    onEchoFrame(src, msdu, len);
  }
}

}  // namespace linktest
