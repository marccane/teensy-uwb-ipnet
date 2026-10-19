#include "hostlink.h"

#include <Arduino.h>
#include <string.h>

#include "app.h"
#include "cobs.h"
#include "console.h"
#include "frame.h"
#include "radio.h"
#include "stats.h"

namespace hostlink {

namespace {

constexpr size_t kMaxCommand = 255;
constexpr size_t kMaxResponse = 2048;
constexpr size_t kMaxIn = 1 + frame::kMaxPayload;  // largest valid decoded message
constexpr size_t kMaxOut = 1 + kMaxResponse;
constexpr uint32_t kReadBudget = 4096;  // bytes per poll(), so the radio is not starved

uint8_t inEnc[cobs::maxEncodedLen(kMaxIn)];
size_t inEncLen = 0;
bool discarding = false;  // oversized message, skip to the next delimiter

// Decoded message. The type byte is overwritten with the radio dispatch byte,
// so an IP packet goes to the radio without copying.
uint8_t msg[kMaxIn];
size_t msgLen = 0;
bool msgPending = false;  // DATA waiting for room in the radio queue

bool hostWasOpen = false;

uint8_t outRaw[kMaxOut];
uint8_t outEnc[cobs::maxEncodedLen(kMaxOut) + 1];

// A RESP is never dropped while the port is open: it waits here, and input
// is not read, until the USB buffer has room for it.
uint8_t respEnc[cobs::maxEncodedLen(kMaxOut) + 1];
size_t respLen = 0;
bool respPending = false;

// Print target that collects command output for a RESP message.
class BufferPrint : public Print {
 public:
  BufferPrint(char* buf, size_t cap) : buf_(buf), cap_(cap) {}
  size_t write(uint8_t c) override {
    if (len_ >= cap_) return 0;
    buf_[len_++] = c;
    return 1;
  }
  size_t length() const { return len_; }

 private:
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
};

// Encodes type + payload with its delimiter into out; returns the length.
size_t encodeMessage(uint8_t type, const uint8_t* payload, size_t len, uint8_t* out) {
  outRaw[0] = type;
  memcpy(outRaw + 1, payload, len);
  size_t n = cobs::encode(outRaw, len + 1, out);
  out[n++] = 0;
  return n;
}

bool sendMessage(uint8_t type, const uint8_t* payload, size_t len) {
  if (!Serial.dtr()) return false;
  const size_t n = encodeMessage(type, payload, len, outEnc);
  if (Serial.availableForWrite() < (int)n) return false;
  Serial.write(outEnc, n);
  return true;
}

// Returns true once no response is waiting.
bool flushResponse() {
  if (!respPending) return true;
  if (!Serial.dtr()) {
    respPending = false;  // nobody to answer
    return true;
  }
  if (Serial.availableForWrite() < (int)respLen) return false;
  Serial.write(respEnc, respLen);
  respPending = false;
  return true;
}

// Returns false if the packet has to wait for queue space.
bool queueIp() {
  if (!radio::ready() || !radio::linkUp()) {
    // Nobody to deliver to: drop now instead of stalling the host (and any
    // command behind this packet) while every frame runs out its retries.
    stats.usbRxDropped++;
    return true;
  }
  if (radio::txQueueFree() == 0) return false;
  const uint8_t* ip = msg + 1;
  const size_t ipLen = msgLen - 1;
  const bool arq = liveConfig().udpArq || !frame::isUdp(ip, ipLen);
  msg[0] = frame::kIp;
  radio::send(msg, msgLen, 0, arq);
  return true;
}

void runCommand() {
  char line[kMaxCommand + 1];
  const size_t n = msgLen - 1 < kMaxCommand ? msgLen - 1 : kMaxCommand;
  memcpy(line, msg + 1, n);
  line[n] = '\0';
  static char out[kMaxResponse];
  BufferPrint p(out, sizeof(out));
  console::exec(line, p);
  respLen = encodeMessage(kResp, (const uint8_t*)out, p.length(), respEnc);
  respPending = true;
}

// Returns false if reading must pause (DATA waiting for queue space, or a
// response waiting for USB buffer space).
bool handleMessage() {
  switch (msg[0]) {
    case kData:
      if (msgLen < 2) {
        stats.usbRxErrors++;
        return true;
      }
      if (!queueIp()) {
        msgPending = true;
        return false;
      }
      return true;
    case kCmd:
      runCommand();
      return flushResponse();
    default:
      stats.usbRxErrors++;
      return true;
  }
}

}  // namespace

void poll() {
  // A new host session starts with a clean decoder, so a half-sent message
  // from the previous one cannot merge with its first message.
  const bool open = Serial.dtr();
  if (open && !hostWasOpen) {
    inEncLen = 0;
    discarding = false;
  }
  hostWasOpen = open;

  if (!flushResponse()) return;
  if (msgPending) {
    if (!queueIp()) return;
    msgPending = false;
  }
  for (uint32_t budget = kReadBudget; budget && Serial.available() > 0; budget--) {
    const int c = Serial.read();
    if (c != 0) {
      if (discarding) continue;
      if (inEncLen < sizeof(inEnc)) {
        inEnc[inEncLen++] = c;
      } else {
        discarding = true;
        stats.usbRxErrors++;
      }
      continue;
    }
    // Delimiter: one complete message (or the end of a discarded one).
    const size_t encLen = inEncLen;
    inEncLen = 0;
    if (discarding) {
      discarding = false;
      continue;
    }
    if (encLen == 0) continue;
    if (!cobs::decode(inEnc, encLen, msg, sizeof(msg), &msgLen) || msgLen == 0) {
      stats.usbRxErrors++;
      continue;
    }
    stats.usbRxFrames++;
    if (!handleMessage()) return;
  }
}

void deliver(const uint8_t* ip, size_t len) {
  if (sendMessage(kData, ip, len)) {
    stats.usbTxFrames++;
  } else {
    stats.usbTxDrops++;
  }
}

}  // namespace hostlink
