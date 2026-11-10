#include "console.h"

#include <Arduino.h>
#include <DW1000.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "frame.h"
#include "linktest.h"
#include "radio.h"
#include "stats.h"
#include "store.h"
#include "version.h"

namespace console {

namespace {

constexpr size_t kMaxArgs = 24;
constexpr uint32_t kSpiTestRounds = 200;

char lineBuf[160];
size_t lineLen = 0;
bool lastWasCr = false;
bool consoleOpen = false;

// Prints text with '\n' line endings as "\r\n" for serial terminals.
void printText(Print& o, const char* text) {
  for (const char* p = text; *p; p++) {
    if (*p == '\n') o.write('\r');
    o.write(*p);
  }
}

bool parseNum(const char* s, int base, uint32_t* out) {
  char* end;
  const unsigned long v = strtoul(s, &end, base);
  if (!*s || *end) return false;
  *out = v;
  return true;
}

int dbm(float v) { return isfinite(v) ? (int)lroundf(v) : -999; }

// The SCK frequency the Teensy 4 LPSPI driver actually produces for a
// request: a whole divider of its peripheral clock, rounded down.
uint32_t actualSpiHz(uint32_t hz) {
  static const uint32_t kClkSel[4] = {664615384, 720000000, 528000000, 396000000};
  const uint32_t cbcmr = CCM_CBCMR;
  const uint32_t clk = kClkSel[(cbcmr >> 4) & 0x03] / (((cbcmr >> 26) & 0x07) + 1);
  uint32_t d = clk / hz;
  if (d && clk / d > hz) d++;
  if (d < 2) d = 2;
  if (d > 257) d = 257;
  return clk / d;
}

void cmdHelp(Print& o) {
  printText(o,
            "info                    firmware, radio and link summary\n"
            "stats [reset]           counters\n"
            "config                  current settings\n"
            "set <key> <value> ...   change settings and apply them; 'set' lists keys\n"
            "save | load | defaults  store settings / reload them / factory settings\n"
            "erase                   forget the stored settings\n"
            "ping [count] [size]     echo test with the peer's firmware\n"
            "flood [seconds] [size]  throughput test towards the peer\n"
            "stop                    stop a running test (also Ctrl-C)\n"
            "spitest [MHz ...]       SPI integrity test, default 2..20 MHz\n"
            "reg <id> [sub] [len]    read a DW1000 register (hex id and sub)\n"
            "reset                   reset and reconfigure the DW1000\n"
            "reboot | bootloader     restart / enter the Teensy bootloader\n");
}

void cmdInfo(Print& o) {
  const Config& c = liveConfig();
  o.printf("uwbnet %s, Teensy 4.0 + DW1000\r\n", UWBNET_VERSION_STRING);
  if (radio::ready()) {
    o.printf("radio:   DEV_ID %08lx, SPI %lu kHz\r\n", radio::deviceId(),
             actualSpiHz(radio::spiClock()) / 1000);
  } else {
    o.printf("radio:   NOT READY (DEV_ID read %08lx, expected deca0130)\r\n", radio::deviceId());
  }
  o.printf("address: %04x, pan %04x, peer ", radio::address(), c.panId);
  if (c.peerAddr == frame::kBroadcast) {
    o.print("broadcast only");
  } else if (radio::peer()) {
    o.printf("%04x (%s)", radio::peer(), c.peerAddr ? "configured" : "learned");
  } else {
    o.print("not heard yet");
  }
  o.printf(", link %s\r\n", radio::linkUp() ? "up" : "down");
  o.printf("phy:     channel %u, %u kbit/s, PRF %u MHz, preamble %u, code %u\r\n", c.channel,
           c.rateKbps, c.prfMhz, c.preambleLen, config::effectivePreambleCode(c));
  o.printf("mac:     MTU %u, %lu us per full frame, ACK timeout %lu us, %u retries%s\r\n",
           (unsigned)frame::kMaxPayload, radio::maxFrameAirtimeUs(), radio::ackTimeoutUs(),
           c.retries, c.udpArq ? "" : " (none for UDP)");
  o.printf("uptime:  %lu s\r\n", millis() / 1000);
}

void cmdStats(Print& o, int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "reset")) {
    memset(&stats, 0, sizeof(stats));
    o.print("counters cleared\r\n");
    return;
  }
  const Stats& s = stats;
  o.printf("tx:   %lu frames: %lu acked, %lu no-ack, %lu failed; %lu attempts, %lu bytes\r\n",
           s.txFrames, s.txAcked, s.txNoAck, s.txFailed, s.txAttempts, s.txBytes);
  o.printf("      queue full %lu, channel defers %lu, stuck %lu, queue free %u\r\n",
           s.txQueueFull, s.ccaDefers, s.txStuck, (unsigned)radio::txQueueFree());
  o.printf("rx:   %lu frames, %lu bytes, %lu duplicates, %lu acks, %lu bad\r\n", s.rxFrames,
           s.rxBytes, s.rxDup, s.rxAcks, s.rxBadFrame);
  o.printf("      errors: phr %lu, fcs %lu, sync %lu, sfd timeout %lu, timeout %lu\r\n",
           s.rxErrPhr, s.rxErrFcs, s.rxErrSync, s.rxErrSfdTimeout, s.rxTimeouts);
  o.printf("      filtered (other PAN/address) %lu\r\n", s.rxFiltered);
  o.printf("      receiver restarts %lu, radio resets %lu, pll flags %lu\r\n", s.rxRestarts,
           s.radioResets, s.pllLossFlags);
  o.printf("usb:  %lu in, %lu bad, %lu dropped (no link), %lu out, %lu dropped\r\n",
           s.usbRxFrames, s.usbRxErrors, s.usbRxDropped, s.usbTxFrames, s.usbTxDrops);
  o.printf("link: rssi %d dBm, first path %d dBm, %lu beacons heard", dbm(s.rssiDbm),
           dbm(s.firstPathDbm), s.helloRx);
  if (s.lastRxMs) o.printf(", last frame %lu ms ago", millis() - s.lastRxMs);
  o.printf("\r\ntest: %lu frames, %lu bytes received\r\n", s.testRxFrames, s.testRxBytes);
}

void cmdConfig(Print& o) {
  const Config& c = liveConfig();
  o.printf("channel %u\r\nprf %u\r\nrate %u\r\nplen %u\r\n", c.channel, c.prfMhz, c.rateKbps,
           c.preambleLen);
  o.printf("pcode %u\r\npan %04x\r\naddr %04x\r\npeer %04x\r\n", c.preambleCode, c.panId,
           c.shortAddr, c.peerAddr);
  o.printf("retries %u\r\nudparq %u\r\nbackoff %u\r\ntxpower %08lx\r\nspi %lu\r\nipqueue %u\r\n",
           c.retries, c.udpArq, c.backoffUs, c.txPower, c.spiHz / 1000000, c.ipQueue);
}

void apply(Print& o, const Config& c) {
  linktest::stop();
  if (applyConfig(c)) {
    o.print("applied ('save' keeps it across reboots)\r\n");
  } else {
    o.print("error: the DW1000 did not come back up, see 'info'\r\n");
  }
}

void cmdSet(Print& o, int argc, char** argv) {
  if (argc == 1) {
    printText(o, config::kKeysHelp);
    o.print("Settings marked 'both ends' must match on the peer.\r\n");
    return;
  }
  if ((argc - 1) % 2) {
    o.print("error: expected key/value pairs\r\n");
    return;
  }
  Config c = liveConfig();
  for (int i = 1; i < argc; i += 2) {
    if (const char* err = config::set(c, argv[i], argv[i + 1])) {
      o.printf("error: %s: %s\r\n", argv[i], err);
      return;
    }
  }
  if (const char* err = config::validate(c)) {
    o.printf("error: %s\r\n", err);
    return;
  }
  apply(o, c);
}

void cmdSpiTest(Print& o, int argc, char** argv) {
  static const uint32_t kDefaultMhz[] = {2, 4, 6, 8, 10, 12, 16, 20};
  uint32_t mhz[kMaxArgs];
  size_t n = 0;
  if (argc > 1) {
    for (int i = 1; i < argc; i++) {
      if (!parseNum(argv[i], 10, &mhz[n]) || mhz[n] < 1 || mhz[n] > 20) {
        o.printf("error: %s: expected 1..20 (MHz)\r\n", argv[i]);
        return;
      }
      n++;
    }
  } else {
    for (uint32_t m : kDefaultMhz) mhz[n++] = m;
  }
  linktest::stop();
  for (size_t i = 0; i < n; i++) {
    uint32_t us = 0;
    const uint32_t hz = mhz[i] * 1000000;
    const uint32_t errors = radio::spiTest(hz, kSpiTestRounds, &us);
    o.printf("%2lu MHz (actual %5lu kHz): %-4s %lu errors in %lu rounds, 1 KB read %lu us\r\n",
             mhz[i], actualSpiHz(hz) / 1000, errors ? "FAIL" : "ok", errors, kSpiTestRounds, us);
  }
  if (!applyConfig(liveConfig())) o.print("error: radio did not come back up\r\n");
}

void cmdReg(Print& o, int argc, char** argv) {
  uint32_t id, sub = NO_SUB, len = 4;
  if (argc < 2 || !parseNum(argv[1], 16, &id) || id > 0x3F ||
      (argc > 2 && (!parseNum(argv[2], 16, &sub) || sub > 0x7FFF)) ||
      (argc > 3 && (!parseNum(argv[3], 10, &len) || len < 1 || len > 64))) {
    o.print("usage: reg <id 0..3f> [sub (hex)] [len 1..64]\r\n");
    return;
  }
  uint8_t b[64];
  radio::readRegister(id, sub, b, len);
  o.printf("%02lx", id);
  if (sub != NO_SUB) o.printf(":%02lx", sub);
  o.print(" =");
  for (uint32_t i = 0; i < len; i++) o.printf(" %02x", b[i]);
  if (len == 4) {
    o.printf("  (0x%08lx)", (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24)));
  }
  o.print("\r\n");
}

void cmdLinkTest(Print& o, int argc, char** argv, bool flood) {
  uint32_t a = flood ? 5 : 10;
  uint32_t size = flood ? frame::kMaxMsdu : 64;
  if ((argc > 1 && !parseNum(argv[1], 10, &a)) || (argc > 2 && !parseNum(argv[2], 10, &size)) ||
      a == 0 || a > 3600) {
    o.printf("usage: %s\r\n", flood ? "flood [seconds] [size]" : "ping [count] [size]");
    return;
  }
  if (!radio::ready()) {
    o.print("error: radio not ready\r\n");
    return;
  }
  if (!radio::peer()) o.print("note: no peer known yet, frames go out as broadcast without ACKs\r\n");
  if (flood) {
    linktest::startFlood(a * 1000, size);
  } else {
    linktest::startPing(a, size);
  }
}

void prompt() { SerialUSB1.print("uwb> "); }

}  // namespace

void exec(char* line, Print& o) {
  char* argv[kMaxArgs];
  int argc = 0;
  for (char* tok = strtok(line, " \t"); tok && argc < (int)kMaxArgs; tok = strtok(nullptr, " \t")) {
    argv[argc++] = tok;
  }
  if (argc == 0) return;
  const char* cmd = argv[0];

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    cmdHelp(o);
  } else if (!strcmp(cmd, "info")) {
    cmdInfo(o);
  } else if (!strcmp(cmd, "stats")) {
    cmdStats(o, argc, argv);
  } else if (!strcmp(cmd, "config")) {
    cmdConfig(o);
  } else if (!strcmp(cmd, "set")) {
    cmdSet(o, argc, argv);
  } else if (!strcmp(cmd, "save")) {
    store::save(liveConfig());
    o.print("saved\r\n");
  } else if (!strcmp(cmd, "load")) {
    Config c;
    if (store::load(&c)) {
      apply(o, c);
    } else {
      o.print("no valid stored settings\r\n");
    }
  } else if (!strcmp(cmd, "defaults")) {
    apply(o, config::defaults());
  } else if (!strcmp(cmd, "erase")) {
    store::erase();
    o.print("stored settings erased; defaults apply after reboot\r\n");
  } else if (!strcmp(cmd, "ping")) {
    cmdLinkTest(o, argc, argv, false);
  } else if (!strcmp(cmd, "flood")) {
    cmdLinkTest(o, argc, argv, true);
  } else if (!strcmp(cmd, "stop")) {
    linktest::stop();
  } else if (!strcmp(cmd, "spitest")) {
    cmdSpiTest(o, argc, argv);
  } else if (!strcmp(cmd, "reg")) {
    cmdReg(o, argc, argv);
  } else if (!strcmp(cmd, "reset")) {
    apply(o, liveConfig());
  } else if (!strcmp(cmd, "reboot")) {
    o.print("rebooting\r\n");
    delay(100);
    SCB_AIRCR = 0x05FA0004;
  } else if (!strcmp(cmd, "bootloader")) {
    o.print("entering bootloader\r\n");
    delay(100);
    _reboot_Teensyduino_();
  } else {
    o.printf("unknown command '%s', try 'help'\r\n", cmd);
  }
}

void poll() {
  const bool open = SerialUSB1.dtr();
  if (open && !consoleOpen) {
    SerialUSB1.printf("\r\nuwbnet %s console, 'help' lists commands\r\n", UWBNET_VERSION_STRING);
    prompt();
  }
  consoleOpen = open;

  while (SerialUSB1.available() > 0) {
    const int c = SerialUSB1.read();
    if (c == '\r' || c == '\n') {
      const bool crlf = c == '\n' && lastWasCr;
      lastWasCr = c == '\r';
      if (crlf) continue;
      SerialUSB1.print("\r\n");
      lineBuf[lineLen] = '\0';
      exec(lineBuf, SerialUSB1);
      lineLen = 0;
      prompt();
      continue;
    }
    lastWasCr = false;
    if (c == 0x03) {  // Ctrl-C
      linktest::stop();
      lineLen = 0;
      SerialUSB1.print("^C\r\n");
      prompt();
    } else if (c == 0x08 || c == 0x7F) {
      if (lineLen) {
        lineLen--;
        SerialUSB1.print("\b \b");
      }
    } else if (c >= 0x20 && c < 0x7F && lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
      SerialUSB1.write(c);
    }
  }
}

}  // namespace console
