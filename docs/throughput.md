# Throughput investigation (2026-11-10)

Why the link delivers about 4 Mbit/s although the DW1000 runs at 6.8 Mbit/s,
what was tried to close the gap, and what came of it.

## Summary

| | Before | After | |
|---|---|---|---|
| Radio only (`flood`, 1012-byte frames) | 3.82 Mbit/s | **4.04 Mbit/s** | +6 % |
| TCP, iperf3 A→B / B→A | 2.93-3.05 / 2.94-2.97 Mbit/s | **2.88-2.97 / 2.97-3.06 Mbit/s** | same within noise |
| UDP offered 5 Mbit/s, delivered | 3.35 Mbit/s | **3.49 Mbit/s** | +4 % |
| UDP at 3 Mbit/s | 0 % loss | 0 % loss | |
| Idle ping | 2.1 ms | 1.8 ms | |

- **Kept:** preloading the next frame into the chip while waiting for the
  ACK, with the pause decided by the ACK's "frame pending" bit. Preload runs
  only for one-way traffic, and only while the ACK is still in its preamble.
- **Dropped:** DW1000 double RX buffering. On this chip it corrupts receptions
  whenever the TX buffer is written at the same time, which happens all the
  time in a two-way link.
- **The remaining limits:**
  - one-way: the receiver has to read each 1 KB frame out over SPI before it
    can receive the next;
  - TCP: every TCP ACK costs a full radio exchange.

## Setup

- Two boards, A and B, on one desk, RSSI about -67 dBm.
- Channel 5, 64 MHz PRF, 6.8 Mbit/s, preamble 128, SPI 20 MHz on both.
- Both ends on one PC, each in its own unprivileged network namespace
  (`tools/nspair.sh`), so IP traffic really crosses the radio.
- Tests:
  - `flood 10`: a console test from board to board, no host involved.
  - iperf3 3.22 runs of 10-12 s; TCP in both directions (`-R`).
  - UDP at 3 Mbit/s and at 5 Mbit/s (overload), with ping running alongside
    to see latency under load.
- Run-to-run spread is about ±0.1 Mbit/s for TCP and ±0.02 for `flood`;
  differences smaller than that are noise.
- The `opt` console command switches the features at runtime, so each
  comparison ran on the same firmware, a few seconds apart.

## Where the time goes

A full frame cycle in `flood` took 2.12 ms (4739 frames in 10.03 s):

| Part | µs | Share |
|---|---|---|
| Payload, 1012 bytes at 6.8 Mbit/s | 1189 | 56 % |
| 802.15.4 header and FCS (11 bytes) | 13 | 1 % |
| Preamble (128 symbols), SFD and PHR, fixed for every frame | 160 | 8 % |
| Hardware ACK: turnaround and ACK frame | 175 | 8 % |
| Gap: writing the next 1 KB into the chip (450 µs at 20 MHz) and firmware | ~580 | 27 % |

The 6.8 Mbit/s already includes the Reed-Solomon overhead, so that is not
lost a second time.

**Ceilings for full frames:**
- **5.9 Mbit/s** back to back, without ACKs or gaps.
- **5.3 Mbit/s** with ACKs.
- **About 4.4 Mbit/s** while the receiver has to read each frame out before
  re-arming. Reading 1 KB at 20 MHz takes ~450 µs, so the next frame can't
  start sooner than ~470 µs after the previous one ends.

SPI already runs at the DW1000's maximum of 20 MHz, so its time can only be
overlapped with something else, not shortened.

## What was tried

### 1. Preload the next frame during the ACK wait

**Idea:** while frame N waits for its ACK, write frame N+1 into the TX buffer.
If N has to be retransmitted, write N again (rare on a good link).

**One-way:**
- Only +3.7 % at first: 3.96 Mbit/s with the existing 300 µs pause after
  every frame.
- Only one 256-byte chunk fits in the ~175 µs ACK wait, and the pause still
  dominated.
- With the pause at 0 or 150 µs: **4.12 Mbit/s (+8 %)**, which is then limited
  by writing the rest of the frame.

**TCP: much worse,** 2.30 / 2.10 Mbit/s instead of 2.93 / 2.94, with 43 %
radio retransmissions and 55 frames given up. Making the sender 75 µs faster
moved its next transmission onto the moment the receiver starts its TCP ACK
(~0.5-0.6 ms after a full frame). Neither side can see the other's preamble
in time, so they collide.

The same effect had shown up earlier in the `backoff` sweep. That was the
old behaviour, a pause after every frame, TCP A→B:

| `backoff` | 0 | 150 | 300 | **600** | 900 µs |
|---|---|---|---|---|---|
| TCP | 2.71 | 3.13 | 3.03 | **2.29** | 2.72 Mbit/s |
| radio retransmissions | 27 % | 5 % | 5 % | **53 %** | 3 % |

**Lesson:** with no real carrier sense, any timing that makes both ends start
together is very costly. A pause that is fixed and the same for both ends is
fragile.

### 2. DW1000 double RX buffering

**Idea:** the chip has two RX buffer sets. The receiver can be re-armed as
soon as the hardware ACK is out, while the previous frame is still being read.
That would remove the ~470 µs receiver limit.

Implemented following Decawave's reference driver (`dwt_isr`,
`dwt_syncrxbufptrs`):
- clear the RX-good events first;
- re-arm without syncing the pointers;
- read the frame in 128-byte chunks, re-arming as soon as the ACK's TXFRS
  shows up;
- toggle HRBPT when done;
- sync the pointers on resets and errors;
- treat RXOVRR as an error.

**Alone:** 3.78 Mbit/s, no gain, because the sender wasn't fast enough yet for
the receiver to be the limit.

**Together with preload:** collapse, 4 kbit/s and 697 of 703 frames failed.
- The receiver got every frame and ACKed it.
- The sender's chip rejected almost every ACK, counted as frame-filter
  rejections ("filtered" in `stats`).
- Writing only the tail of the TX buffer, never its first 256 bytes, made no
  difference.
- So with double buffering on, **any write to the TX buffer while a frame is
  arriving spoils that frame.**

**TCP without preload:** 2.72 / 2.73 Mbit/s (-7 %), with ~40 rejected frames
per board per 10 s. In a two-way link each board writes its next frame into
the TX buffer whenever it can, often while the peer's frame is arriving.

**Conclusion:** unusable here, because the TX buffer has to be written while
receiving. It is off by default; `opt dblbuf on` still enables it for
experiments.

### 3. Pause only when the peer has data ("frame pending")

**Idea:**
- The receiver sets AACKPEND (SYS_CFG bit 31) while its transmit queue is
  non-empty, so its hardware ACKs carry the 802.15.4 "frame pending" bit.
- The sender pauses `backoff` µs after an ACK with that bit set, and
  otherwise sends at once.
- The pause has to cover the peer's turnaround (~0.5 ms at 20 MHz SPI).
- The default became 800 µs, which leaves room for boards that fall back to a
  slower SPI clock.

**Results:**
- Preload works one way without hurting TCP: `flood` 4.02 Mbit/s.
- TCP 2.96 / 2.97 Mbit/s for pauses of 600, 800 and 1000 µs alike.
- Control, with the bit never set (`opt ackpend off`), so the sender never
  pauses: TCP 2.01 Mbit/s with preload and 2.44 Mbit/s without, 40-60 % radio
  retransmissions. Some form of yielding is essential.

**New side effect:** `flood` showed 112 retransmissions instead of ~7, and the
receiver counted ~300 duplicates. A duplicate means the receiver got the frame
and ACKed it, but the sender missed the ACK. For `flood` the cause was the
preload: even in single-buffer mode, a TX-buffer write that overlaps the
arriving ACK sometimes spoils it, a mild version of the double-buffer failure.
Experiment 4 fixed that. The TCP duplicates have another cause (see below).

### 4. Time the preload against the ACK

**Idea:** the ACK's content only arrives after its preamble, SFD and PHR,
about 160 µs after our frame at 6.8 Mbit/s and about 2 ms at 110 kbit/s. Write
a preload chunk only if it will finish before then. Both times are computed
from the PHY settings and the SPI clock. At 6.8 Mbit/s and 20 MHz that allows
exactly one 256-byte chunk, started at once.

**One-way:** 4.05 Mbit/s with **9 retransmissions** (was 112) and 1 duplicate.

**TCP:** 2.88 / 2.88 Mbit/s (two runs), against 2.97 / 2.97 with preload off,
about -3 %. In TCP the limit is the turn-taking, not the sender's gap, so a
faster sender mainly delays the peer's TCP ACKs.

### 5. Preload only for one-way traffic (final)

If the last ACK carried "frame pending", the peer has data, and the next
frame is not preloaded.

| Test | Result |
|---|---|
| `flood` | 4.04 Mbit/s, 12 retransmissions, 1 duplicate |
| TCP, four runs | 2.97 / 3.06, 2.97 / 3.06, 2.96 / 3.06, 2.88 / 2.97 Mbit/s |
| UDP at 3 Mbit/s | 0 % loss |
| UDP at 5 Mbit/s (overload) | 3.49 Mbit/s delivered, ping 49 ms |
| Idle ping | 1.8 ms |
| Ping while TCP runs | ~20 ms |

## Trade-offs and remaining limits

- **TCP is capped at about 3.1 Mbit/s by turn-taking.** Two data segments
  (2 x ~2 ms) plus one TCP ACK exchange (preamble, small frame, radio ACK and
  turnarounds, ~0.9 ms) carry 2 x 959 bytes of payload. Faster data frames
  hardly matter; fewer or cheaper TCP ACK exchanges would.
- **One-way is capped at about 4.4 Mbit/s** by the receiver reading each
  frame out. Double buffering would have lifted that cap, but this chip
  doesn't allow it here.
- **Radio ACKs stay on by default.** They cost ~175 µs per frame, about 8 %.
  Without them every collision or bit error reaches TCP as loss.
  `set udparq 0` sends UDP without them; `set retries 0` turns them off
  completely.
- **Duplicates during TCP:** the receiver counts 2-5 % of frames twice, i.e.
  the sender missed the receiver's ACK and sent the frame again. The firmware
  from before this work had ~0.2 %. Measured:
  - not the preload: ~250-330 per test with it off as well;
  - not the AACKPEND register writes: ~350 with them off and preload off;
  - the count varies a lot between runs (300-850 per pair of 12 s tests).

  Most likely a timing race that only exists without the old fixed pause, but
  not identified. It costs some airtime, not throughput: TCP is the same as
  before. TCP never sees the copies, because the receiver drops them.

## Ideas not tried

- **Aggregation:** pack several queued IP packets into one radio frame, up to
  1011 bytes. TCP ACKs are ~52 bytes and usually queue up at the receiver, so
  sending them together would save whole exchanges. This is the most promising
  idea for TCP.
- **Preamble of 64 symbols** (`set plen 64`, both ends): saves 65 µs per
  frame, about +3 %, at some cost in range.
- **Shorter SPI chip-select hold** in the driver (`delayMicroseconds(5)` per
  transaction, ~15 transactions per frame exchange): about +3 %.
- **Writing the next frame behind the transmit pointer during TX:** the SPI
  is faster than the air, so it is possible in principle. Undocumented, and
  risky after what double buffering showed.

## Queueing (measured earlier the same day)

The other knob is how many packets may wait. The board takes up to `ipqueue`
IP packets from the host. Beyond that they wait in the host's interface
queue, 8 packets by default (`uwbnetd -q`).

Without these limits, two buffers on the host side added up to a second of
delay under UDP overload:
- the TUN interface's default queue of 500 packets;
- the `cdc_acm` USB driver, which keeps up to 16 transfers in flight.
  `uwbnetd` now keeps only one transfer pending.

Measured with the earlier MAC (fixed 300 µs pause). TCP shown A→B:

| `ipqueue` | 2 | 4 | **8** | 16 |
|---|---|---|---|---|
| TCP A→B, Mbit/s | 2.62 | 2.79 | 2.96 | 3.14 |
| ping while TCP runs | 11 ms | 17 ms | 23 ms | 37 ms |
| ping under UDP overload | 37 ms | 41 ms | 51 ms | 69 ms |

8 gives up ~5 % of TCP throughput for a third less delay under load.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `backoff` | 800 µs | pause after an ACK that says the peer has data |
| `ipqueue` | 8 | IP packets that may wait in the board |
| `opt preload` | on | preload for one-way traffic, timed against the ACK |
| `opt ackpend` | on | set "frame pending" in our auto-ACKs while frames are queued |
| `opt dblbuf` | off | double RX buffering, for experiments only |

`opt` settings are not saved; they reset to these defaults at boot.
