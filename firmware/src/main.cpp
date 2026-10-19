// IP-over-UWB modem for a Teensy 4.0 with a DW1000.
//
// USB port 1 (Serial): COBS-framed IP packets and commands for the host
// daemon. USB port 2 (SerialUSB1): text console.

#include <Arduino.h>

#include "app.h"
#include "config.h"
#include "console.h"
#include "frame.h"
#include "hostlink.h"
#include "linktest.h"
#include "radio.h"
#include "stats.h"
#include "store.h"

Stats stats;

namespace {

Config live;

void onRadioFrame(uint16_t src, const uint8_t* msdu, size_t len) {
  switch (msdu[0]) {
    case frame::kIp:
      hostlink::deliver(msdu + 1, len - 1);
      break;
    case frame::kTest:
    case frame::kEcho:
      linktest::onFrame(src, msdu, len);
      break;
    default:
      stats.rxBadFrame++;
      break;
  }
}

}  // namespace

const Config& liveConfig() { return live; }

bool applyConfig(const Config& cfg) {
  live = cfg;
  return radio::reconfigure(live);
}

void setup() {
  Serial.begin(0);  // USB CDC; the baud rate is ignored
  SerialUSB1.begin(0);
  if (!store::load(&live)) live = config::defaults();
  radio::setRxHandler(onRadioFrame);
  // On failure the console stays up; 'info' and 'spitest' help find out why.
  radio::begin(live);
}

void loop() {
  radio::poll();
  hostlink::poll();
  linktest::poll();
  console::poll();
}
