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
constexpr uint16_t kSysCfgAckPendBit = 31;  // AACKPEND: set "frame pending" in our auto-ACKs

constexpr uint32_t kCtrlTxStart = 1u << 1;
constexpr uint32_t kCtrlTrxOff = 1u << 6;
constexpr uint32_t kCtrlWait4Resp = 1u << 7;
constexpr uint32_t kCtrlRxEnable = 1u << 8;
constexpr uint32_t kCtrlHrbpt = 1u << 24;  // toggle the host-side RX buffer (double buffering)

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
constexpr uint32_t kStRxOvrr = 1u << 20;  // both RX buffers full
constexpr uint32_t kStRxPto = 1u << 21;
constexpr uint32_t kStRfPllLl = 1u << 24;
constexpr uint32_t kStClkPllLl = 1u << 25;
constexpr uint32_t kStRxSfdTo = 1u << 26;
constexpr uint32_t kStAffRej = 1u << 29;
constexpr uint32_t kStHsrbp = 1u << 30;  // host-side RX buffer pointer
constexpr uint32_t kStIcrbp = 1u << 31;  // IC-side RX buffer pointer

constexpr uint32_t kStTxAll = kStAat | kStTxFrb | kStTxPrs | kStTxPhs | kStTxFrs;
constexpr uint32_t kStRxGood = kStRxPrd | kStRxSfdd | kStLdeDone | kStRxPhd | kStRxDfr | kStRxFcg;
constexpr uint32_t kStRxErr = kStRxPhe | kStRxFce | kStRxRfsl | kStRxSfdTo | kStAffRej | kStLdeErr;
constexpr uint32_t kStRxTimeout = kStRxRfto | kStRxPto;
constexpr uint32_t kStRxInProgress = kStRxPrd | kStRxSfdd | kStRxPhd;
constexpr uint32_t kStPll = kStRfPllLl | kStClkPllLl;
// Frame-filter rejections (AFFREJ) are handled like errors, as Decawave's
// driver does, rather than relying on the receiver re-arming by itself.
constexpr uint32_t kStRxErrIrq =
    kStRxPhe | kStRxFce | kStRxRfsl | kStRxSfdTo | kStAffRej | kStRxOvrr | kStRxTimeout;
// TX status bits other than AAT. AAT belongs to the received frame and is only
// cleared in onRxGood(), so a frame completing mid-handler keeps it.
constexpr uint32_t kStTxDone = kStTxFrb | kStTxPrs | kStTxPhs | kStTxFrs;

// Events that raise the IRQ line.
constexpr uint32_t kIrqMask = kStTxFrs | kStRxFcg | kStRxErrIrq;

constexpr uint32_t kDevIdDw1000 = 0xDECA0130;
static_assert(config::kMaxIpQueue <= kTxQueueLen, "ipqueue cannot exceed the radio queue");
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
// Margin when timing preload writes against the ACK's arrival.
constexpr uint32_t kPreloadMarginUs = 15;
// Bytes rewritten after an auto-ACK. Unknown whether the ACK touches the TX
// buffer at all; if it does, it can only have overwritten the start.
constexpr uint16_t kDirtyRewriteLen = 16;
// RX buffer bytes read per SPI transaction while an auto-ACK is going out, so
// the receiver can be re-armed as soon as the ACK is done.
constexpr uint16_t kRxChunk = 128;

struct TxSlot {
  uint16_t len;
  uint16_t dst;
  bool arq;
  uint8_t msdu[frame::kMaxMsdu];
};

TxSlot txq[kTxQueueLen];
size_t txHead = 0;
size_t txCount = 0;

// A queued frame turned into a PSDU. Kept until the frame is acked or given
// up, so retransmissions reuse it.
struct TxFrame {
  bool built;
  bool wantAck;
  uint8_t seq;
  uint8_t attempts;
  uint16_t psduLen;  // including FCS
  uint32_t airUs;
  uint8_t psdu[frame::kMaxPsdu];
};

// cur is the queue head (in flight); nxt the frame after it, prepared while
// cur waits for its ACK. They swap when cur completes.
TxFrame frames[2];
TxFrame* cur = &frames[0];
TxFrame* nxt = &frames[1];

// What the DW1000 TX buffer holds: the kLoadChunk-sized chunks of bufFrame's
// PSDU whose bits are set in bufChunks.
const TxFrame* bufFrame = nullptr;
uint8_t bufChunks = 0;

enum class Outcome { kAcked, kSentNoAck, kFailed };

Config cfg;
Tuning tune;
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
uint32_t ownTxDoneUs = 0;      // when we noticed our frame was out
uint32_t ackContentUs = 0;     // after our frame ends, when the ACK's bytes start arriving
uint32_t chunkWriteUs = 0;     // time to write one kLoadChunk at the current SPI clock
bool autoAckActive = false;  // the chip is sending an ACK for a frame we received
uint32_t autoAckStartUs = 0;
bool txBufferDirty = false;  // an auto-ACK went out since the TX buffer was loaded
bool ackPendBit = false;     // AACKPEND as last written to SYS_CFG
bool peerHadData = false;    // the last ACK we got carried "frame pending"
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

// Double buffering: hands the buffer just read back to the receiver.
void releaseRxBuffer() { writeCtrl(kCtrlHrbpt); }

// Double buffering: points host and IC at the same buffer, dropping any frame
// not yet read (Decawave's dwt_syncrxbufptrs). Only call with the receiver
// off and no frame being read.
void syncRxBuffers() {
  if (!tune.dblBuf) return;
  const uint32_t st = readStatus();
  if (!(st & kStIcrbp) != !(st & kStHsrbp)) releaseRxBuffer();
}

// Turn the receiver off, reset it and re-arm it. Required after receive
// errors, and after an auto-ACK sent while WAIT4RESP was active (the chip
// would otherwise re-apply WAIT4RESP).
void restartRx() {
  writeCtrl(kCtrlTrxOff);
  clearStatus(kStRxGood | kStRxErr | kStRxTimeout);
  uint8_t v = kSoftResetRx;
  DW1000.writeBytes(PMSC, kSubPmscSoftReset, &v, 1);
  v = kSoftResetClear;
  DW1000.writeBytes(PMSC, kSubPmscSoftReset, &v, 1);
  syncRxBuffers();
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
  DW1000.setDoubleBuffering(tune.dblBuf);
  DW1000.setBit(DW1000._syscfg, LEN_SYS_CFG, kSysCfgAckPendBit, false);
  ackPendBit = false;
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
  // An empty PSDU's airtime is the ACK's preamble + SFD + PHR.
  ackContentUs = phy::frameAirtimeUs(mode, 0) - kPreloadMarginUs;
  // 8 bits per byte plus per-transaction overhead (header, CS hold time).
  chunkWriteUs = (uint32_t)((uint64_t)kLoadChunk * 8 * 1000000 / DW1000Class::getSPIClock()) + 15;

  clearStatus(kStTxAll | kStRxGood | kStRxErr | kStRxTimeout | kStPll);
  syncRxBuffers();
  rxEnable();
}

uint32_t readDevId() { return readReg32(DEV_ID); }

void resetMacState() {
  txActive = ackPending = autoAckActive = false;
  txBufferDirty = deferring = false;
  cur->built = nxt->built = false;
  bufFrame = nullptr;  // the TX buffer is gone after a reset
  txNotBeforeUs = micros();
}

// peerWaiting: the peer's ACK said it has frames to send (or we can't tell).
void completeCurrent(Outcome outcome, bool peerWaiting = true) {
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
  cur->built = false;
  TxFrame* done = cur;  // nxt, possibly already in the TX buffer, moves up
  cur = nxt;
  nxt = done;
  deferring = false;
  lastOwnTxMs = millis();
  // If the peer has something to send, leave it a window before our next
  // frame. It skips this pause after receiving from us, so traffic
  // alternates when both have data. Otherwise go on at once.
  txNotBeforeUs = micros() + (peerWaiting ? cfg.backoffUs + randBelow(kGuardJitterUs) : 0);
}

// Our auto-ACKs carry "frame pending" while we have frames queued, so the
// peer pauses for us only when we actually want to send (see completeCurrent).
void updateAckPendBit() {
  const bool want = tune.ackPend && txCount > 0;
  if (want == ackPendBit) return;
  DW1000.setBit(DW1000._syscfg, LEN_SYS_CFG, kSysCfgAckPendBit, want);
  DW1000.writeSystemConfigurationRegister();
  ackPendBit = want;
}

// Turns queue slot `slot` into a PSDU in f. Frames are built in queue order,
// so sequence numbers stay in transmit order.
void buildFrame(TxFrame* f, size_t slot) {
  const TxSlot& s = txq[slot];
  const uint16_t dst = s.dst ? s.dst : currentPeer();
  f->wantAck = s.arq && cfg.retries > 0 && dst != frame::kBroadcast;
  f->seq = nextSeq++;
  const size_t hl = frame::writeDataHeader(f->psdu, f->seq, cfg.panId, dst, myAddr, f->wantAck);
  memcpy(f->psdu + hl, s.msdu, s.len);
  f->psduLen = hl + s.len + frame::kFcsLen;
  f->attempts = 0;
  f->airUs = phy::frameAirtimeUs(mode, f->psduLen);
  f->built = true;
  if (bufFrame == f) bufFrame = nullptr;  // the buffer held this slot's previous frame
}

// Writes one more chunk of f to the TX buffer, replacing whatever frame was
// there. Returns true once all of f is there. Chunking keeps poll()
// responsive to receive events. With head=false the first chunk is left
// alone: it still holds the frame waiting for its ACK (see preloadNext).
bool loadTxBuffer(const TxFrame* f, bool head = true) {
  const uint16_t dataLen = f->psduLen - frame::kFcsLen;
  const uint8_t chunks = (dataLen + kLoadChunk - 1) / kLoadChunk;
  const uint8_t all = (1u << chunks) - 1;
  if (bufFrame != f) {
    bufFrame = f;
    bufChunks = 0;
    txBufferDirty = false;
  }
  if (txBufferDirty) {
    // A short rewrite, so that replying right after an auto-ACK stays quick.
    if (bufChunks & 1) {
      const uint16_t n = dataLen < kDirtyRewriteLen ? dataLen : kDirtyRewriteLen;
      DW1000.writeBytes(TX_BUFFER, 0, const_cast<uint8_t*>(f->psdu), n);
    }
    txBufferDirty = false;
  }
  const uint8_t todo = all & ~bufChunks & (head ? 0xFF : 0xFE);
  if (todo) {
    const uint8_t c = __builtin_ctz(todo);
    const uint16_t off = c * kLoadChunk;
    const uint16_t n = dataLen - off < kLoadChunk ? dataLen - off : kLoadChunk;
    DW1000.writeBytes(TX_BUFFER, off, const_cast<uint8_t*>(f->psdu) + off, n);
    bufChunks |= 1u << c;
  }
  return bufChunks == all;
}

// While cur waits for its ACK, put the next frame into the TX buffer. If cur
// has to be retransmitted it is written again (rare on a good link).
//
// Writing the TX buffer while the ACK's bytes arrive spoils the ACK: always
// with double buffering on, now and then without (measured: duplicates at
// the peer). The ACK's preamble, SFD and PHR come first (~160 us at
// 6.8 Mbit/s, ~2 ms at 110 kbit/s), so chunks are written only while they
// finish before that. The first chunk (frame control, sequence number) waits
// until after the ACK.
//
// Only for one-way traffic: when the peer has data too (TCP), a faster sender
// mainly delays the peer's replies (measured: TCP -3 %, one-way +6 %).
void preloadNext() {
  if (!tune.preload || txCount < 2 || peerHadData) return;
  if (micros() - ownTxDoneUs + chunkWriteUs > ackContentUs) return;
  if (!nxt->built) buildFrame(nxt, (txHead + 1) % kTxQueueLen);
  if (bufFrame != nxt && bufFrame == cur) {
    // Keep cur's first chunk; the other chunks become nxt's.
    bufFrame = nxt;
    bufChunks = 0;
  }
  loadTxBuffer(nxt, false);
}

void startTx() {
  writeCtrl(kCtrlTrxOff);
  uint8_t fctrl[LEN_TX_FCTRL];
  memcpy(fctrl, txFctrlTemplate, sizeof(fctrl));
  fctrl[0] = cur->psduLen & 0xFF;
  fctrl[1] = (fctrl[1] & 0xE0) | ((cur->psduLen >> 8) & 0x03);
  DW1000.writeBytes(TX_FCTRL, NO_SUB, fctrl, LEN_TX_FCTRL);
  clearStatus(kStTxAll | kStRxGood | kStRxErr | kStRxTimeout);
  syncRxBuffers();  // the ACK we wait for must land in the buffer we read next
  writeCtrl(kCtrlTxStart | (cur->wantAck ? kCtrlWait4Resp : 0));
  txActive = true;
  txStartUs = micros();
  cur->attempts++;
  stats.txAttempts++;
}

void tryTransmit() {
  if (!cur->built) buildFrame(cur, txHead);
  if (!loadTxBuffer(cur)) return;
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
  if (cur->wantAck) {
    // WAIT4RESP has already turned the receiver on.
    ackPending = true;
    ownTxDoneUs = micros();
    ackDeadlineUs = ownTxDoneUs + ackTimeout;
  } else {
    rxEnable();
    completeCurrent(Outcome::kSentNoAck);
  }
}

void onAckTimeout() {
  ackPending = false;
  if (cur->attempts > cfg.retries) {
    completeCurrent(Outcome::kFailed);
    return;
  }
  // Exponential backoff sized in frame exchanges, so two nodes that collided
  // spread their retries apart. The receiver is still on from WAIT4RESP.
  const uint32_t shift = cur->attempts > 4 ? 3 : cur->attempts - 1;
  txNotBeforeUs = micros() + randBelow((cur->airUs + ackTimeout) << shift);
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
    const bool awaiting = ackPending || (cur->built && cur->wantAck && cur->attempts && !txActive);
    if (awaiting && h.seq == cur->seq) {
      ackPending = false;
      peerHadData = h.framePending;
      completeCurrent(Outcome::kAcked, h.framePending);
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

// Double buffering: the auto-ACK is out, so the receiver can take the next
// frame into the other buffer while this one is still being read.
void rearmAfterAutoAck() {
  const uint32_t st = readStatus();
  if (!(st & kStTxFrs)) return;
  clearStatus(st & kStTxDone);
  autoAckActive = false;
  rxEnable();  // no pointer sync: the host-side buffer is still being read
}

void onRxGood(uint32_t st) {
  // Clear the receive events first (as dwt_isr does): once the receiver is
  // re-armed, new events belong to the next frame.
  clearStatus(kStRxGood | kStRxErr | kStRxTimeout);

  uint8_t finfo[LEN_RX_FINFO];
  DW1000.readBytes(RX_FINFO, NO_SUB, finfo, LEN_RX_FINFO);
  const uint16_t psduLen = (finfo[0] | (finfo[1] << 8)) & 0x3FF;
  const uint16_t n = psduLen > frame::kFcsLen ? psduLen - frame::kFcsLen : 0;
  uint16_t got = n < kRxChunk ? n : kRxChunk;
  if (got) DW1000.readBytes(RX_BUFFER, NO_SUB, rxBuf, got);

  // The chip sends the ACK by itself; it must finish before the receiver is
  // touched again. AAT can also be left over from an earlier frame, so check
  // that this one asked for an ACK. Read the bits directly rather than from
  // the parser: the chip ACKs anything its filter accepts.
  const bool ackOut = (st & kStAat) && n >= 1 && (rxBuf[0] & 0x07) == frame::kTypeData &&
                      (rxBuf[0] & frame::kFcAckRequest);
  if ((st & kStAat) && !ackOut) clearStatus(kStAat);
  if (ackOut) {
    autoAckActive = true;
    autoAckStartUs = micros();
    txBufferDirty = true;
  } else if (tune.dblBuf) {
    rxEnable();  // the next frame goes to the other buffer
  }

  if (tune.dblBuf && autoAckActive) {
    // Read in chunks and re-arm the receiver as soon as the ACK is out.
    while (got < n) {
      const uint16_t c = n - got < kRxChunk ? n - got : kRxChunk;
      DW1000.readBytes(RX_BUFFER, got, rxBuf + got, c);  // offsets are multiples of 128, never NO_SUB
      got += c;
      if (autoAckActive && digitalReadFast(board::kDwIrq)) rearmAfterAutoAck();
    }
    if (autoAckActive) rearmAfterAutoAck();
  } else if (got < n) {
    DW1000.readBytes(RX_BUFFER, got, rxBuf + got, n - got);
  }

  frame::Header h;
  const size_t hl = frame::parse(rxBuf, n, &h);
  const bool isData = hl && h.type == frame::kTypeData;
  // Signal quality: these registers belong to the buffer being read (they are
  // double-buffered too), valid until it is released or the receiver re-armed
  // in single-buffer mode. Sample control frames and every 16th IP packet.
  if (isData && (n == hl || rxBuf[hl] != frame::kIp || (ipRxCount++ & 15) == 0)) {
    stats.rssiDbm = DW1000.getReceivePower();
    stats.firstPathDbm = DW1000.getFirstPathPower();
  }

  if (tune.dblBuf) {
    releaseRxBuffer();
  } else if (!ackOut) {
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
  if (st & kStRxOvrr) stats.rxOverruns++;
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
  if (txActive && now - txStartUs > cur->airUs + kTxWatchdogMarginUs) {
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

Tuning& tuning() { return tune; }

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
  if (txCount && !txActive && !ackPending && !autoAckActive) {
    tryTransmit();
  } else if (ackPending && !autoAckActive) {
    preloadNext();
  }
  updateAckPendBit();
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
size_t txQueued() { return txCount; }
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
