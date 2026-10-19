# Local changes to the vendored DW1000 driver

Upstream: https://github.com/thotro/arduino-dw1000 at commit `922ca3e`
(2020-05-11). Only the register-level driver is vendored (`DW1000`,
`DW1000Time`, constants); `DW1000Ranging`, `DW1000Device` and `DW1000Mac` are
not used.

The first commit in this repository is the unmodified upstream copy, so
`git log -p -- firmware/lib/DW1000` shows every change below.

1. **Runtime SPI clock** (`setSPIClock()` / `getSPIClock()`). `_fastSPI` is no
   longer `const`. The firmware uses this to fall back to a slower clock when
   the board fails an SPI integrity check, and for the `spitest` command.
   The DW1000 allows up to 20 MHz.
2. **Buffered SPI transfers on Teensy 4** (`readBytes()` / `writeBytes()`).
   Uses `SPI.transfer(buf, retbuf, n)` instead of one call per byte, which keeps
   the LPSPI FIFO busy for the 1 KB TX/RX buffer accesses.

Not changed, but worth knowing:

- `begin()` still attaches `handleInterrupt()` as an ISR. The firmware detaches
  it right after `begin()` and polls the IRQ line from `loop()` instead, so no
  SPI traffic ever happens in interrupt context.
- `handleInterrupt()` and the `newReceive()`/`startTransmit()` helpers are not
  used. The firmware drives `SYS_CTRL`/`SYS_STATUS` itself (see
  `src/radio.cpp`) because upstream clears every status bit at the end of the
  handler, which can drop an event that arrives while the handler runs.
