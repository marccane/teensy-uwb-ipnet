#include "radio.h"

#include <Arduino.h>
#include <DW1000.h>
#include <string.h>

#include "board.h"
#include "frame.h"
#include "log.h"
#include "phy.h"
#include "stats.h"
#include "version.h"

// Event handling follows Decawave's reference driver (dwt_isr) rather than the
// thotro handleInterrupt(): status bits are cleared individually, receive
// errors get a receiver reset, and the auto-ACK quirks are handled explicitly.
// Everything runs from loop(); the IRQ line is polled, never used as an ISR.

namespace radio {

namespace {

// Registers and bits the driver has no names for (DW1000 user manual, ch. 7).
constexpr uint8_t kRegAckRespT = 0x1A;
constexpr uint16_t kSubDrxSfdToc = 0x20;      // DRX_CONF (driver name: DRX_TUNE)
constexpr uint16_t kSubPmscSoftReset = 0x03;  // PMSC_CTRL0, byte 3
constexpr uint8_t kSoftResetRx = 0xE0;
constexpr uint8_t kSoftResetClear = 0xF0;
constexpr uint16_t kSysCfgAutoAckBit = 30;

constexpr uint32_t kCtrlTxStart = 1u << 1;
constexpr uint32_t kCtrlTrxOff = 1u << 6;
constexpr uint32_t kCtrlWait4Resp = 1u << 7;
constexpr uint32_t kCtrlRxEnable = 1u << 8;

constexpr uint32_t kStAat = 1u << 3;
constexpr uint32_t kStTxFrb = 1u << 4;
constexpr uint32_t kStTxPrs = 1u << 5;
constexpr uint32_t kStTxPhs = 1u << 6;
constexpr uint32_t kStTxFrs = 1u << 7;
constexpr uint32_t kStRxPrd = 1u << 8;
constexpr uint32_t kStRxSfdd = 1u << 9;
constexpr uint32_t kStLdeDone = 1u << 10;
constexpr uint32_t kStRxPhd = 1u << 11;
constexpr uint32_t kStRxPhe = 1u << 12;
constexpr uint32_t kStRxDfr = 1u << 13;
constexpr uint32_t kStRxFcg = 1u << 14;
constexpr uint32_t kStRxFce = 1u << 15;
constexpr uint32_t kStRxRfsl = 1u << 16;
constexpr uint32_t kStRxRfto = 1u << 17;
constexpr uint32_t kStLdeErr = 1u << 18;
constexpr uint32_t kStRxPto = 1u << 21;
constexpr uint32_t kStRfPllLl = 1u << 24;
constexpr uint32_t kStClkPllLl = 1u << 25;
constexpr uint32_t kStRxSfdTo = 1u << 26;
constexpr uint32_t kStAffRej = 1u << 29;

constexpr uint32_t kStTxAll = kStAat | kStTxFrb | kStTxPrs | kStTxPhs | kStTxFrs;
constexpr uint32_t kStRxGood = kStRxPrd | kStRxSfdd | kStLdeDone | kStRxPhd | kStRxDfr | kStRxFcg;
constexpr uint32_t kStRxErr = kStRxPhe | kStRxFce | kStRxRfsl | kStRxSfdTo | kStAffRej | kStLdeErr;
constexpr uint32_t kStRxTimeout = kStRxRfto | kStRxPto;
constexpr uint32_t kStRxInProgress = kStRxPrd | kStRxSfdd | kStRxPhd;
constexpr uint32_t kStPll = kStRfPllLl | kStClkPllLl;
// Frame-filter rejections (AFFREJ) are handled like errors, as Decawave's
// driver does, rather than relying on the receiver re-arming by itself.
constexpr uint32_t kStRxErrIrq =
    kStRxPhe | kStRxFce | kStRxRfsl | kStRxSfdTo | kStAffRej | kStRxTimeout;
// TX status bits other than AAT. AAT belongs to the received frame and is only
// cleared in onRxGood(), so a frame completing mid-handler keeps it.
constexpr uint32_t kStTxDone = kStTxFrb | kStTxPrs | kStTxPhs | kStTxFrs;

// Events that raise the IRQ line.
constexpr uint32_t kIrqMask = kStTxFrs | kStRxFcg | kStRxErrIrq;

constexpr uint32_t kDevIdDw1000 = 0xDECA0130;
constexpr uint16_t kOtpXtalTrim = 0x01E;

constexpr uint32_t kAckMarginUs = 300;          // ACK wait beyond its airtime
constexpr uint32_t kTxWatchdogMarginUs = 5000;  // TX done must arrive within airtime + this
constexpr uint32_t kAutoAckMarginUs = 500;
constexpr uint32_t kGuardJitterUs = 64;
constexpr uint32_t kHelloIntervalMs = 1000;
constexpr uint32_t kLinkTimeoutMs = 3500;
constexpr uint32_t kRxIdleRestartMs = 3000;
constexpr uint32_t kRecoverIntervalMs = 5000;  // retry a radio that failed to come up
constexpr uint16_t kLoadChunk = 256;  // TX buffer bytes written per poll(); never 0xFF-aligned
// Bytes rewritten after an auto-ACK. Unknown whether the ACK touches the TX
// buffer at all; if it does, it can only have overwritten the start.
constexpr uint16_t kDirtyRewriteLen = 16;

struct TxSlot {
  uint16_t len;
  uint16_t dst;
  bool arq;
  uint8_t msdu[frame::kMaxMsdu];
};

TxSlot txq[kTxQueueLen];
size_t txHead = 0;
size_t txCount = 0;

// The queue head once it has been turned into a PSDU. Kept until the frame is
// acked or given up, so retransmissions reuse it.
struct {
  bool built;
  bool wantAck;
  uint8_t seq;
  uint8_t attempts;
  uint16_t psduLen;  // including FCS
  uint16_t loaded;   // bytes already written to the DW1000 TX buffer
  uint32_t airUs;
  uint8_t psdu[frame::kMaxPsdu];
} cur;

enum class Outcome { kAcked, kSentNoAck, kFailed };

Config cfg;
bool isReady = false;
bool notFoundLogged = false;
uint32_t lastRecoverMs = 0;
uint32_t devId = 0;
uint16_t myAddr = 0;
uint16_t learnedPeer = 0;
RxHandler rxHandler = nullptr;
phy::Mode mode;
uint8_t txFctrlTemplate[LEN_TX_FCTRL];
uint8_t nextSeq = 0;
uint32_t ackTimeout = 0;
uint32_t maxAirtime = 0;
uint32_t autoAckTimeout = 0;

bool txActive = false;  // our frame is on air
uint32_t txStartUs = 0;
bool ackPending = false;  // our frame went out, waiting for its ACK
uint32_t ackDeadlineUs = 0;
bool autoAckActive = false;  // the chip is sending an ACK for a frame we received
uint32_t autoAckStartUs = 0;
bool txBufferDirty = false;  // an auto-ACK went out since the TX buffer was loaded
bool deferring = false;
uint32_t deferStartUs = 0;
uint32_t txNotBeforeUs = 0;

bool lastRxValid = false;
uint16_t lastRxSrc = 0;
uint8_t lastRxSeq = 0;
uint32_t lastRxEventMs = 0;
uint32_t lastPeerRxMs = 0;
bool peerHeard = false;
bool linkWasUp = false;
uint32_t lastOwnTxMs = 0;
bool helloQueued = false;
uint32_t ipRxCount = 0;

uint8_t rxBuf[frame::kMaxPsdu];
uint32_t rngState = 1;

uint32_t rand32() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return rngState;
}

uint32_t randBelow(uint32_t n) { return n ? rand32() % n : 0; }

bool timeReached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

uint32_t get32(const uint8_t* b) {
  return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

void put32(uint8_t* b, uint32_t v) {
  b[0] = v;
  b[1] = v >> 8;
  b[2] = v >> 16;
  b[3] = v >> 24;
}

uint32_t readReg32(uint8_t reg) {
  uint8_t b[4];
  DW1000.readBytes(reg, NO_SUB, b, 4);
  return get32(b);
}

void writeReg32(uint8_t reg, uint32_t v) {
  uint8_t b[4];
  put32(b, v);
  DW1000.writeBytes(reg, NO_SUB, b, 4);
}

uint32_t readStatus() { return readReg32(SYS_STATUS); }
void clearStatus(uint32_t bits) { writeReg32(SYS_STATUS, bits); }
void writeCtrl(uint32_t bits) { writeReg32(SYS_CTRL, bits); }
void rxEnable() { writeCtrl(kCtrlRxEnable); }

// Turn the receiver off, reset it and re-arm it. Required after receive
// errors, and after an auto-ACK sent while WAIT4RESP was active (the chip
// would otherwise re-apply WAIT4RESP).
void restartRx() {
  writeCtrl(kCtrlTrxOff);
  uint8_t v = kSoftResetRx;
  DW1000.writeBytes(PMSC, kSubPmscSoftReset, &v, 1);
  v = kSoftResetClear;
  DW1000.writeBytes(PMSC, kSubPmscSoftReset, &v, 1);
  clearStatus(kStRxGood | kStRxErr | kStRxTimeout);
  rxEnable();
  deferring = false;  // whatever we were deferring to is gone
}

uint16_t factoryAddress() {
  const uint32_t mac = HW_OCOTP_MAC0;
  uint16_t a = mac & 0xFFFF;
  if (a == 0 || a >= 0xFFFE) a = ((mac >> 16) & 0x7FFF) | 1;
  return a;
}

uint16_t currentPeer() {
  if (cfg.peerAddr) return cfg.peerAddr;
  return learnedPeer ? learnedPeer : frame::kBroadcast;
}

byte rateCode(uint16_t kbps) {
  switch (kbps) {
    case 110:
      return DW1000Class::TRX_RATE_110KBPS;
    case 850:
      return DW1000Class::TRX_RATE_850KBPS;
    default:
      return DW1000Class::TRX_RATE_6800KBPS;
  }
}

byte preambleLenCode(uint16_t len) {
  switch (len) {
    case 64:
      return DW1000Class::TX_PREAMBLE_LEN_64;
    case 256:
      return DW1000Class::TX_PREAMBLE_LEN_256;
    case 512:
      return DW1000Class::TX_PREAMBLE_LEN_512;
    case 1024:
      return DW1000Class::TX_PREAMBLE_LEN_1024;
    case 1536:
      return DW1000Class::TX_PREAMBLE_LEN_1536;
    case 2048:
      return DW1000Class::TX_PREAMBLE_LEN_2048;
    case 4096:
      return DW1000Class::TX_PREAMBLE_LEN_4096;
    default:
      return DW1000Class::TX_PREAMBLE_LEN_128;
  }
}

// Full register setup after a reset. The driver's tune() supplies the
// channel/PRF/rate dependent values from the user manual.
void configureChip() {
  DW1000.newConfiguration();
  DW1000.setDefaults();
  DW1000.setDeviceAddress(myAddr);
  DW1000.setNetworkId(cfg.panId);
  DW1000.setPulseFrequency(cfg.prfMhz == 16 ? DW1000Class::TX_PULSE_FREQ_16MHZ
                                            : DW1000Class::TX_PULSE_FREQ_64MHZ);
  DW1000.setDataRate(rateCode(cfg.rateKbps));
  DW1000.setPreambleLength(preambleLenCode(cfg.preambleLen));
  DW1000.setChannel(cfg.channel);
  DW1000.setPreambleCode(config::effectivePreambleCode(cfg));
  DW1000.useExtendedFrameLength(true);
  DW1000.useSmartPower(false);
  DW1000.suppressFrameCheck(false);
  DW1000.setReceiverAutoReenable(false);
  DW1000.setFrameFilter(true);
  DW1000.setFrameFilterAllowData(true);
  DW1000.setFrameFilterAllowAcknowledgement(true);
  DW1000.setBit(DW1000._syscfg, LEN_SYS_CFG, kSysCfgAutoAckBit, true);
  DW1000.commitConfiguration();

  // tune() reads the factory crystal trim from OTP while running on the PLL;
  // Decawave's driver forces the crystal clock for OTP reads to get reliable
  // values, so read it again that way and rewrite FS_XTALT.
  uint8_t otp[LEN_OTP_RDAT];
  DW1000.enableClock(DW1000Class::XTI_CLOCK);
  DW1000.readBytesOTP(kOtpXtalTrim, otp);
  DW1000.enableClock(DW1000Class::AUTO_CLOCK);
  const uint8_t trim = (otp[0] & 0x1F) ? (otp[0] & 0x1F) : 0x10;  // 0x10 = mid-range
  uint8_t xtalt = trim | 0x60;  // bits 5-6 must be written as 1
  DW1000.writeBytes(FS_CTRL, FS_XTALT_SUB, &xtalt, LEN_FS_XTALT);

  writeReg32(SYS_MASK, kIrqMask);
  // ACK_RESP_T: auto-ACK turnaround (ACK_TIM, preamble symbols) in the top
  // byte; W4R_TIM = 0 so the receiver opens right after our own TX.
  writeReg32(kRegAckRespT, (uint32_t)(cfg.rateKbps == 6800 ? 3 : 0) << 24);
  // SFD detection timeout. The reset value (4161 symbols) would keep the
  // receiver hunting for ~4 ms after every false preamble detection.
  const uint16_t sfdTimeout =
      cfg.preambleLen + phy::sfdSymbols(cfg.rateKbps) + 1 - phy::pacSize(cfg.preambleLen);
  uint8_t b[2] = {(uint8_t)(sfdTimeout & 0xFF), (uint8_t)(sfdTimeout >> 8)};
  DW1000.writeBytes(DRX_TUNE, kSubDrxSfdToc, b, 2);
  if (cfg.txPower) writeReg32(TX_POWER, cfg.txPower);

  memcpy(txFctrlTemplate, DW1000._txfctrl, sizeof(txFctrlTemplate));
  mode = {cfg.rateKbps, cfg.prfMhz, cfg.preambleLen};
  const uint32_t ackAir = phy::frameAirtimeUs(mode, frame::kAckLen + frame::kFcsLen);
  ackTimeout = ackAir + kAckMarginUs;
  autoAckTimeout = ackAir + kAutoAckMarginUs;
  maxAirtime = phy::frameAirtimeUs(mode, frame::kMaxPsdu);

  clearStatus(kStTxAll | kStRxGood | kStRxErr | kStRxTimeout | kStPll);
  rxEnable();
}

uint32_t readDevId() { return readReg32(DEV_ID); }

void resetMacState() {
  txActive = ackPending = autoAckActive = false;
  txBufferDirty = deferring = false;
  cur.built = false;  // the TX buffer is gone after a reset
  txNotBeforeUs = micros();
}

void completeCurrent(Outcome outcome) {
  const TxSlot& s = txq[txHead];
  stats.txFrames++;
  switch (outcome) {
    case Outcome::kAcked:
      stats.txAcked++;
      stats.txBytes += s.len;
      break;
    case Outcome::kSentNoAck:
      stats.txNoAck++;
      stats.txBytes += s.len;
      break;
    case Outcome::kFailed:
      stats.txFailed++;
      break;
  }
  if (s.msdu[0] == frame::kHello) helloQueued = false;
  txHead = (txHead + 1) % kTxQueueLen;
  txCount--;
  cur.built = false;
  deferring = false;
  lastOwnTxMs = millis();
  // Leave the peer a window to reply before our next frame. It skips this
  // pause after receiving from us, so traffic alternates when both have data.
  txNotBeforeUs = micros() + cfg.backoffUs + randBelow(kGuardJitterUs);
}

void buildCurrent() {
  const TxSlot& s = txq[txHead];
  const uint16_t dst = s.dst ? s.dst : currentPeer();
  cur.wantAck = s.arq && cfg.retries > 0 && dst != frame::kBroadcast;
  cur.seq = nextSeq++;
  const size_t hl = frame::writeDataHeader(cur.psdu, cur.seq, cfg.panId, dst, myAddr, cur.wantAck);
  memcpy(cur.psdu + hl, s.msdu, s.len);
  cur.psduLen = hl + s.len + frame::kFcsLen;
  cur.loaded = 0;
  cur.attempts = 0;
  cur.airUs = phy::frameAirtimeUs(mode, cur.psduLen);
  cur.built = true;
}

// Writes the next chunk of the frame to the TX buffer. Returns true once the
// whole frame is there. Chunking keeps poll() responsive to receive events.
bool loadTxBuffer() {
  if (txBufferDirty) {
    // A short rewrite, so that replying right after an auto-ACK stays quick.
    const uint16_t n = cur.loaded < kDirtyRewriteLen ? cur.loaded : kDirtyRewriteLen;
    if (n) DW1000.writeBytes(TX_BUFFER, 0, cur.psdu, n);
    txBufferDirty = false;
  }
  const uint16_t dataLen = cur.psduLen - frame::kFcsLen;
  if (cur.loaded < dataLen) {
    const uint16_t left = dataLen - cur.loaded;
    const uint16_t n = left < kLoadChunk ? left : kLoadChunk;
    DW1000.writeBytes(TX_BUFFER, cur.loaded, cur.psdu + cur.loaded, n);
    cur.loaded += n;
  }
  return cur.loaded >= dataLen;
}

void startTx() {
  writeCtrl(kCtrlTrxOff);
  uint8_t fctrl[LEN_TX_FCTRL];
  memcpy(fctrl, txFctrlTemplate, sizeof(fctrl));
  fctrl[0] = cur.psduLen & 0xFF;
  fctrl[1] = (fctrl[1] & 0xE0) | ((cur.psduLen >> 8) & 0x03);
  DW1000.writeBytes(TX_FCTRL, NO_SUB, fctrl, LEN_TX_FCTRL);
  clearStatus(kStTxAll | kStRxGood | kStRxErr | kStRxTimeout);
  writeCtrl(kCtrlTxStart | (cur.wantAck ? kCtrlWait4Resp : 0));
  txActive = true;
  txStartUs = micros();
  cur.attempts++;
  stats.txAttempts++;
}

void tryTransmit() {
  if (!cur.built) buildCurrent();
  if (!loadTxBuffer()) return;
  const uint32_t now = micros();
  if (!timeReached(now, txNotBeforeUs)) return;

  // Poor man's clear-channel check: don't start while a frame is arriving.
  const uint32_t st = readStatus();
  if (st & kIrqMask) return;  // an event is pending; handle it first
  if (st & kStRxInProgress) {
    if (!deferring) {
      deferring = true;
      deferStartUs = now;
      stats.ccaDefers++;
    }
    // Stale detection bits would block us forever; give up after one frame time.
    if (now - deferStartUs < maxAirtime + 1000) return;
  }
  deferring = false;
  startTx();
}

void onOwnTxDone() {
  txActive = false;
  if (cur.wantAck) {
    // WAIT4RESP has already turned the receiver on.
    ackPending = true;
    ackDeadlineUs = micros() + ackTimeout;
  } else {
    rxEnable();
    completeCurrent(Outcome::kSentNoAck);
  }
}

void onAckTimeout() {
  ackPending = false;
  if (cur.attempts > cfg.retries) {
    completeCurrent(Outcome::kFailed);
    return;
  }
  // Exponential backoff sized in frame exchanges, so two nodes that collided
  // spread their retries apart. The receiver is still on from WAIT4RESP.
  const uint32_t shift = cur.attempts > 4 ? 3 : cur.attempts - 1;
  txNotBeforeUs = micros() + randBelow((cur.airUs + ackTimeout) << shift);
}

void notePeer(uint16_t src) {
  if (cfg.peerAddr == 0 && learnedPeer != src) {
    learnedPeer = src;
    logPrintf("peer is %04x", src);
  }
  if (src == currentPeer() || cfg.peerAddr == frame::kBroadcast) {
    lastPeerRxMs = millis();
    peerHeard = true;
  }
}

void handleFrame(const frame::Header& h, const uint8_t* msdu, size_t len) {
  if (h.type == frame::kTypeAck) {
    stats.rxAcks++;
    // Also accept an ACK that arrives after the timeout, while the retry is
    // still waiting out its backoff.
    const bool awaiting = ackPending || (cur.built && cur.wantAck && cur.attempts && !txActive);
    if (awaiting && h.seq == cur.seq) {
      ackPending = false;
      completeCurrent(Outcome::kAcked);
    }
    return;
  }
  if (len == 0 || h.src == myAddr) {
    stats.rxBadFrame++;
    return;
  }
  // The sender has one frame in flight, so a retransmission always repeats
  // the frame received just before it. Track every data frame (sequence
  // numbers are shared with unacked ones) but only drop repeats of acked ones.
  const bool dup = h.ackRequest && lastRxValid && h.src == lastRxSrc && h.seq == lastRxSeq;
  lastRxValid = true;
  lastRxSrc = h.src;
  lastRxSeq = h.seq;
  if (dup) {
    stats.rxDup++;  // our ACK got lost; the hardware has acked it again
    return;
  }
  if (h.ackRequest) {
    // The sender now pauses for us (see completeCurrent), so we may reply at once.
    txNotBeforeUs = micros();
  }
  notePeer(h.src);
  stats.rxFrames++;
  stats.rxBytes += len;
  stats.lastRxMs = millis();
  if (msdu[0] == frame::kHello) {
    stats.helloRx++;
    return;
  }
  if (rxHandler) rxHandler(h.src, msdu, len);
}

void onRxGood(uint32_t st) {
  uint8_t finfo[LEN_RX_FINFO];
  DW1000.readBytes(RX_FINFO, NO_SUB, finfo, LEN_RX_FINFO);
  const uint16_t psduLen = (finfo[0] | (finfo[1] << 8)) & 0x3FF;
  const uint16_t n = psduLen > frame::kFcsLen ? psduLen - frame::kFcsLen : 0;
  if (n) DW1000.readBytes(RX_BUFFER, NO_SUB, rxBuf, n);

  frame::Header h;
  const size_t hl = frame::parse(rxBuf, n, &h);
  const bool isData = hl && h.type == frame::kTypeData;
  // The chip sends the ACK by itself; it must finish before the receiver is
  // touched again. AAT can also be left over from an earlier frame, so check
  // that this one asked for an ACK. Read the bits directly rather than from
  // the parser: the chip ACKs anything its filter accepts.
  const bool ackOut = (st & kStAat) && n >= 1 && (rxBuf[0] & 0x07) == frame::kTypeData &&
                      (rxBuf[0] & frame::kFcAckRequest);

  // Signal quality registers are only valid until the receiver is re-armed.
  // Sample control frames and every 16th IP packet.
  if (isData && (n == hl || rxBuf[hl] != frame::kIp || (ipRxCount++ & 15) == 0)) {
    stats.rssiDbm = DW1000.getReceivePower();
    stats.firstPathDbm = DW1000.getFirstPathPower();
  }

  clearStatus(kStRxGood | kStRxErr | kStRxTimeout | (ackOut ? 0 : kStAat));
  if (ackOut) {
    autoAckActive = true;
    autoAckStartUs = micros();
    txBufferDirty = true;
  } else {
    rxEnable();
  }
  deferring = false;
  lastRxEventMs = millis();

  if (!hl) {
    stats.rxBadFrame++;
    return;
  }
  handleFrame(h, rxBuf + hl, n - hl);
}

void onRxError(uint32_t st) {
  if (st & kStRxPhe) stats.rxErrPhr++;
  if (st & kStRxFce) stats.rxErrFcs++;
  if (st & kStRxRfsl) stats.rxErrSync++;
  if (st & kStRxSfdTo) stats.rxErrSfdTimeout++;
  if (st & kStAffRej) stats.rxFiltered++;
  if (st & kStRxTimeout) stats.rxTimeouts++;
  restartRx();
  lastRxEventMs = millis();
}

void handleEvents() {
  const uint32_t st = readStatus();
  if (st & kStPll) {
    stats.pllLossFlags++;
    clearStatus(st & kStPll);
  }
  // One TXFRS bit can stand for our own frame and an auto-ACK sent after it.
  // Settle our own frame first so an ACK in the same batch finds ackPending.
  // Only bits seen in st are cleared, so nothing that arrives meanwhile is lost.
  bool txfrsUsed = false;
  if ((st & kStTxFrs) && txActive) {
    clearStatus(st & kStTxDone);
    onOwnTxDone();
    txfrsUsed = true;
  }
  if (st & kStRxFcg) {
    onRxGood(st);
  } else if (st & kStRxErrIrq) {
    onRxError(st);
  }
  if ((st & kStTxFrs) && !txfrsUsed) {
    clearStatus(st & kStTxDone);
    if (autoAckActive) {
      autoAckActive = false;
      restartRx();
    }
  }
}

void resetRadio() {
  stats.radioResets++;
  if (!reconfigure(cfg)) logPrintf("radio reset failed");
}

void checkTimers() {
  const uint32_t now = micros();
  if (ackPending && timeReached(now, ackDeadlineUs)) onAckTimeout();
  if (txActive && now - txStartUs > cur.airUs + kTxWatchdogMarginUs) {
    stats.txStuck++;
    logPrintf("TX did not complete, resetting radio");
    resetRadio();
    return;
  }
  if (autoAckActive && now - autoAckStartUs > autoAckTimeout) {
    // Its TXFRS was consumed together with our own frame's (see handleEvents).
    autoAckActive = false;
    clearStatus(kStTxDone);
    restartRx();
  }
}

void housekeeping() {
  const uint32_t ms = millis();
  if (!helloQueued && ms - lastOwnTxMs >= kHelloIntervalMs) {
    const uint8_t hello[] = {frame::kHello, UWBNET_VERSION_MAJOR, UWBNET_VERSION_MINOR};
    helloQueued = send(hello, sizeof(hello), frame::kBroadcast, false);
    lastOwnTxMs = ms;
  }
  const bool up = linkUp();
  if (up != linkWasUp) {
    linkWasUp = up;
    if (up) {
      logPrintf("link up, peer %04x", currentPeer());
    } else {
      logPrintf("link down");
    }
  }
  // Safety net: if nothing at all has been received for a while, re-arm the
  // receiver in case it was left off.
  if (ms - lastRxEventMs > kRxIdleRestartMs && !txActive && !autoAckActive) {
    stats.rxRestarts++;
    restartRx();
    lastRxEventMs = ms;
  }
}

}  // namespace

bool begin(const Config& c) {
  DW1000.begin(board::kDwIrq, board::kDwRst);
  // The driver attaches its own ISR, which would do SPI in interrupt context.
  detachInterrupt(digitalPinToInterrupt(board::kDwIrq));
  rngState = HW_OCOTP_MAC0 ^ ARM_DWT_CYCCNT ^ micros();
  if (!rngState) rngState = 1;
  nextSeq = rand32();
  return reconfigure(c);
}

bool reconfigure(const Config& c) {
  cfg = c;
  isReady = false;
  myAddr = cfg.shortAddr ? cfg.shortAddr : factoryAddress();
  if (cfg.peerAddr) learnedPeer = 0;
  resetMacState();

  // Try the configured SPI clock, then slower ones.
  const uint32_t ladder[] = {cfg.spiHz, 16000000, 12000000, 8000000, 6000000, 4000000, 2000000};
  uint32_t hz = 0;
  for (uint32_t candidate : ladder) {
    if (candidate > cfg.spiHz) continue;
    DW1000Class::setSPIClock(candidate);
    DW1000.select(board::kDwCs);  // hard reset, loads the LDE microcode
    devId = readDevId();
    if (devId == kDevIdDw1000 && spiTest(candidate, 32, nullptr) == 0) {
      hz = candidate;
      break;
    }
    if (devId == kDevIdDw1000) logPrintf("SPI errors at %lu Hz", candidate);
  }
  lastRecoverMs = millis();
  if (!hz) {
    // poll() keeps retrying; say so once rather than every attempt.
    if (!notFoundLogged) logPrintf("DW1000 not answering (DEV_ID %08lx), will retry", devId);
    notFoundLogged = true;
    return false;
  }
  if (notFoundLogged) logPrintf("DW1000 answering again");
  notFoundLogged = false;
  if (hz != cfg.spiHz) logPrintf("using SPI at %lu Hz", hz);

  DW1000.select(board::kDwCs);  // start from reset values after the test patterns
  configureChip();
  lastRxEventMs = millis();
  isReady = true;
  return true;
}

void setRxHandler(RxHandler handler) { rxHandler = handler; }

void poll() {
  if (!isReady) {
    // A failed reset (or a chip that was not there at boot) is retried.
    if (millis() - lastRecoverMs >= kRecoverIntervalMs) reconfigure(cfg);
    return;
  }
  if (digitalReadFast(board::kDwIrq)) handleEvents();
  checkTimers();
  if (!isReady) return;
  housekeeping();
  if (txCount && !txActive && !ackPending && !autoAckActive) tryTransmit();
}

bool send(const uint8_t* msdu, size_t len, uint16_t dst, bool arq) {
  if (len == 0 || len > frame::kMaxMsdu) return false;
  if (txCount >= kTxQueueLen) {
    stats.txQueueFull++;
    return false;
  }
  TxSlot& s = txq[(txHead + txCount) % kTxQueueLen];
  s.len = len;
  s.dst = dst;
  s.arq = arq;
  memcpy(s.msdu, msdu, len);
  txCount++;
  return true;
}

size_t txQueueFree() { return kTxQueueLen - txCount; }
bool txIdle() { return txCount == 0; }
bool ready() { return isReady; }
uint32_t deviceId() { return devId; }
uint16_t address() { return myAddr; }
uint16_t peer() {
  const uint16_t p = currentPeer();
  return p == frame::kBroadcast ? 0 : p;
}
bool linkUp() { return peerHeard && millis() - lastPeerRxMs < kLinkTimeoutMs; }
uint32_t spiClock() { return DW1000Class::getSPIClock(); }
uint32_t ackTimeoutUs() { return ackTimeout; }
uint32_t maxFrameAirtimeUs() { return maxAirtime; }

uint32_t spiTest(uint32_t hz, uint32_t rounds, uint32_t* bulkReadUs) {
  static uint8_t a[LEN_RX_BUFFER];
  static uint8_t b[LEN_RX_BUFFER];
  isReady = false;
  DW1000Class::setSPIClock(hz);
  writeCtrl(kCtrlTrxOff);
  uint8_t saved[LEN_USR_SFD];
  uint8_t pattern[LEN_USR_SFD];
  uint8_t back[LEN_USR_SFD];
  DW1000.readBytes(USR_SFD, NO_SUB, saved, LEN_USR_SFD);
  uint32_t errors = 0;
  for (uint32_t r = 0; r < rounds; r++) {
    if (readDevId() != kDevIdDw1000) errors++;
    for (uint8_t& p : pattern) p = rand32();
    DW1000.writeBytes(USR_SFD, NO_SUB, pattern, LEN_USR_SFD);
    DW1000.readBytes(USR_SFD, NO_SUB, back, LEN_USR_SFD);
    if (memcmp(pattern, back, LEN_USR_SFD) != 0) errors++;
    if ((r & 7) == 0) {
      // The receive buffer does not change while the radio is off.
      DW1000.readBytes(RX_BUFFER, NO_SUB, a, LEN_RX_BUFFER);
      DW1000.readBytes(RX_BUFFER, NO_SUB, b, LEN_RX_BUFFER);
      if (memcmp(a, b, LEN_RX_BUFFER) != 0) errors++;
    }
  }
  if (bulkReadUs) {
    const uint32_t t0 = micros();
    DW1000.readBytes(RX_BUFFER, NO_SUB, a, LEN_RX_BUFFER);
    *bulkReadUs = micros() - t0;
  }
  DW1000.writeBytes(USR_SFD, NO_SUB, saved, LEN_USR_SFD);
  return errors;
}

void readRegister(uint8_t reg, uint16_t sub, uint8_t* out, uint16_t len) {
  DW1000.readBytes(reg, sub, out, len);
}

}  // namespace radio
