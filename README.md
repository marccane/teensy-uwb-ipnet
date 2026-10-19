# uwbnet: IP over UWB with two Teensy 4.0 + DW1000 boards

Firmware that turns a Teensy 4.0 wired to a Decawave DW1000 into a
USB "UWB modem". Two boards give two Linux hosts a point-to-point IP link.

```
Linux A                           Teensy A       ~~UWB~~       Teensy B                           Linux B
uwb0 (TUN) <-> uwbnetd <-> ttyACM0 <-> fw <-> DW1000  ~~~~~~~~~  DW1000 <-> fw <-> ttyACM0 <-> uwbnetd <-> uwb0 (TUN)
```

Status:

- **Firmware**: written and builds. Not yet run on hardware.
- **Host daemon** (`uwbnetd`, creates the `uwb0` TUN interface): not written
  yet. The USB protocol it will speak is in [docs/protocol.md](docs/protocol.md).
- The console link tests (`ping`, `flood`) exercise the radio link
  board-to-board without any host software.

## Layout

```
firmware/              PlatformIO project (teensy40)
  src/                 radio MAC, USB framing, console, config
  lib/DW1000/          thotro/arduino-dw1000 driver, patched (see PATCHES.md)
  tests/               host-side unit tests for the hardware-independent code
docs/protocol.md       USB framing and over-the-air format
```

## Build and flash

```sh
cd firmware
pio run                  # build
pio run -t upload        # flash (teensy_loader_cli)
make -C tests            # unit tests on the PC
```

The upload asks the running firmware to reboot into the bootloader. If the
board runs something that doesn't support that, press the Teensy's program
button when the loader waits for it. From this firmware, the console command
`bootloader` does the same.

## Hardware notes

- DW1000 wiring (`firmware/src/board.h`): CS 10, RST 9, IRQ 3, on SPI0
  (SCK 13, MOSI 11, MISO 12).
- The firmware uses no other pins. The Teensy LED shares pin 13 with SCK, so
  there is no status LED; use the console.
- SPI starts at 20 MHz (the DW1000 maximum). The firmware checks the bus at
  every radio reset and steps down through 16/12/8/6/4/2 MHz if a check
  fails. `info` shows the clock in use.
- Settings live in the emulated EEPROM from byte 256 on, so whatever another
  firmware keeps in the low bytes survives.

## First run checklist

Open the console on the second port. It prints a banner when opened:

```sh
pio device monitor -p /dev/ttyACM1      # or: screen /dev/ttyACM1, picocom, ...
```

1. `info`: `DEV_ID deca0130` means the DW1000 answers. `NOT READY` means it
   failed at every SPI clock (check power and wiring with `reg 0`).
2. `spitest`: SPI integrity from 2 to 20 MHz. If high clocks fail, pin a
   working one with `set spi 12` and `save` (it falls back automatically, but
   every radio reset then pays for the failed attempts).
3. Flash the second board. Within about a second both consoles log
   `peer is xxxx` and `link up`.
4. `ping 20`: round-trip times and RSSI.
5. `flood 10` on one board: delivered kbit/s. The other board logs what it
   received and how many frames were lost.
6. `stats` on both: retransmissions, receive errors, channel defers.

Range and rate: `set profile mid` (850 kbit/s) or `set profile long`
(110 kbit/s) on **both** boards. Raise TX power with `set txpower <hex>`;
`0` is the DW1000 user manual's reference value for the channel (for channel
5 at 64 MHz PRF that is `85858585`). `1f1f1f1f` is the maximum gain
setting, over 20 dB above that and likely beyond the regulatory limit.

### Assumptions to confirm on hardware

The firmware was written from the DW1000 user manual and Decawave's reference
driver, without boards to test on. These are the places most likely to need
adjusting:

- **Auto-ACK turnaround** (`ACK_TIM` = 3 symbols at 6.8 Mbit/s). If `ping`
  shows retries on every frame at short range, this is the first suspect.
- **Auto-ACK and the TX buffer**: unknown whether the hardware ACK overwrites
  the start of the TX buffer, so the firmware rewrites the first 16 bytes
  after each one. It's harmless either way.
- **Frame-filter rejections** are treated like receive errors (receiver reset),
  as Decawave's driver does. `stats` counts them as "filtered".
- **SPI at 20 MHz** on this board's traces (`spitest`).

## Console

| Command | |
|---|---|
| `info` | firmware, radio, address/peer, link, PHY, MTU |
| `stats [reset]` | counters |
| `config` | settings as `key value` lines |
| `set <key> <value> ...` | change and apply at once; `set` alone lists keys |
| `save` / `load` / `defaults` / `erase` | EEPROM: store / reload / factory values / forget |
| `ping [count] [size]` | echo test against the peer firmware |
| `flood [seconds] [size]` | one-way throughput test |
| `stop` or Ctrl-C | stop a test |
| `spitest [MHz ...]` | SPI check, default 2..20 MHz |
| `reg <id> [sub] [len]` | read a DW1000 register (hex) |
| `reset` | reset and reconfigure the DW1000 |
| `reboot` / `bootloader` | restart / enter the Teensy bootloader |

Defaults: channel 5, PRF 64 MHz, 6.8 Mbit/s, preamble 128 (code 10), PAN
`5557`, peer learned automatically, 3 retries.

## Design notes

**Why the link acknowledges and retransmits even though TCP is reliable.**
TCP reads every lost segment as congestion and halves its sending rate, so
1-2 % random radio loss already costs most of the throughput. Also, UWB has
no real carrier sense, and with TCP both ends transmit (data one way, ACKs the
other), so the two radios collide regularly. A short local retry (hardware
ACK about 170 us after the frame, at most 3 retries) hides that loss from TCP.
Wi-Fi does the same for the same reason. The cost for UDP is a few
milliseconds of extra delay on the rare retried packet. For UDP that would
rather lose than wait, `set udparq 0` sends UDP without ACKs while TCP keeps
them, and `set retries 0` turns ACKs off completely. Running `flood`/`iperf3`
in each mode on the real hardware will show which works best.

**Expected throughput** (6.8 Mbit/s, preamble 128, 1011-byte packets): a full
frame is ~1.36 ms on air plus ~0.17 ms for the ACK. Moving 1 KB over SPI takes
~0.4 ms at 20 MHz and ~1.4 ms at 6 MHz. The firmware loads the next frame
during the pause after each exchange. That predicts roughly 3.5-4 Mbit/s one
way at 20 MHz SPI and ~2.5 Mbit/s at 6 MHz, before IP/TCP overhead. These are
estimates; `flood` gives the real figure.

**Possible later improvements**: DW1000 RX double buffering (receive the next
frame while the previous one is read out), CDC-NCM so no host daemon is
needed, header compression.
