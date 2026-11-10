#pragma once

#include <stdint.h>

// Counters shown by the "stats" command. Single-threaded: only touched from loop().
struct Stats {
  // Radio transmit
  uint32_t txFrames;    // frames completed (acked, sent without ACK, or failed)
  uint32_t txAttempts;  // transmissions started, retries included
  uint32_t txAcked;
  uint32_t txNoAck;     // sent without ACK request (broadcast, ARQ off)
  uint32_t txFailed;    // dropped after exhausting retries
  uint32_t txBytes;     // MSDU bytes of acked and no-ACK frames
  uint32_t txQueueFull;
  uint32_t txStuck;     // TX never completed; radio was reset
  uint32_t ccaDefers;   // transmit postponed because a frame was arriving

  // Radio receive
  uint32_t rxFrames;    // data frames accepted
  uint32_t rxBytes;
  uint32_t rxDup;       // retransmissions of a frame already delivered
  uint32_t rxAcks;
  uint32_t rxBadFrame;  // CRC fine but not a frame this firmware sends
  uint32_t rxErrPhr;
  uint32_t rxErrFcs;
  uint32_t rxErrSync;   // Reed-Solomon sync loss
  uint32_t rxErrSfdTimeout;
  uint32_t rxFiltered;  // rejected by the address filter (other PAN or node)
  uint32_t rxOverruns;  // both RX buffers were full (double buffering)
  uint32_t rxTimeouts;
  uint32_t rxRestarts;  // receiver restarted by the idle watchdog
  uint32_t pllLossFlags;
  uint32_t radioResets;

  // USB host link
  uint32_t usbRxFrames;
  uint32_t usbRxErrors;  // bad COBS, oversize, unknown type
  uint32_t usbRxDropped; // IP packets dropped: radio not ready or link down
  uint32_t usbTxFrames;
  uint32_t usbTxDrops;   // host not reading or port closed

  // Link quality (last measured frame)
  float rssiDbm;
  float firstPathDbm;
  uint32_t lastRxMs;
  uint32_t helloRx;
  uint32_t testRxFrames;
  uint32_t testRxBytes;
};

extern Stats stats;
