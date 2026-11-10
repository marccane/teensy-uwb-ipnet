# uwbnet: IP over UWB with two Teensy 4.0 + DW1000 boards

Firmware that turns a Teensy 4.0 wired to a Decawave DW1000 into a
USB "UWB modem", plus a Linux daemon that exposes it as a network interface.
Two boards give two Linux hosts a point-to-point IP link.

```
Linux A                         Teensy A      ~~UWB~~      Teensy B                         Linux B
uwb0 (TUN) <-> uwbnetd <-> ttyACMx <-> fw <-> DW1000 ~~~~~~~ DW1000 <-> fw <-> ttyACMx <-> uwbnetd <-> uwb0 (TUN)
10.77.0.1                                                                                   10.77.0.2
```

Status: working on two boards (2026-11-10). Measured numbers are under
[Measured performance](#measured-performance).

## Layout

```
firmware/              PlatformIO project (teensy40)
  src/                 radio MAC, USB framing, console, config
  lib/DW1000/          thotro/arduino-dw1000 driver, patched (see PATCHES.md)
  tests/               host-side unit tests for the hardware-independent code
host/uwbnetd.cpp       TUN <-> board daemon
tools/                 uwbcon.py (console from scripts), nspair.sh (both ends
                       on one PC), tput.py (throughput test without iperf3)
docs/protocol.md       USB framing and over-the-air format
```

## Build and flash

```sh
cd firmware
pio run                  # build
make -C tests            # unit tests on the PC
make -C ../host          # the daemon
```

Flashing with **one** Teensy attached: `pio run -t upload`. It asks the running
firmware to reboot into the bootloader (any Teensyduino firmware with USB
serial supports that).

With **several** Teensys attached, `pio run -t upload` may pick any of them.
Send the reboot request to one board's port yourself, then let the loader
take whichever board is in the bootloader:

```sh
stty -F /dev/ttyACM1 134      # this board only (for uwbnet: its packet port)
teensy_loader_cli --mcu=TEENSY40 -w .pio/build/teensy40/firmware.hex
```

The console command `bootloader` does the same as the `stty` step.

## Ports

Each board shows up as two CDC-ACM ports (product `UWB-Net`, serial number =
the Teensy's own):

- interface 00: **packet port**, for `uwbnetd`
- interface 02: **console**, for humans and `tools/uwbcon.py`

`ls -l /dev/serial/by-id/` shows which `ttyACM` is which. Example:
`usb-uwbnet_UWB-Net_12345678-if00` is board 12345678's packet port.

## Running the link

On each host (needs `CAP_NET_ADMIN`, so normally root or a systemd service):

```sh
uwbnetd -a 10.77.0.1/30 /dev/serial/by-id/usb-uwbnet_UWB-Net_<serial>-if00   # host A
uwbnetd -a 10.77.0.2/30 /dev/serial/by-id/usb-uwbnet_UWB-Net_<serial>-if00   # host B
```

Options: `-i` interface name (`uwb0`), `-m` MTU (max and default 1011), `-q`
interface queue length (8), `-v` counters every 10 s. `uwbnetd -c "<command>"
<port>` runs one console command over the packet port.

**Both boards on one PC, without root:** `tools/nspair.sh` puts each end in its
own unprivileged network namespace, so traffic really crosses the radio.

```sh
tools/nspair.sh start /dev/ttyACM1 /dev/ttyACM2   # packet ports of A and B
tools/nspair.sh a ping 10.77.0.2
tools/nspair.sh b iperf3 -s -D
tools/nspair.sh a iperf3 -c 10.77.0.2
tools/nspair.sh stop
```

## Hardware notes

- DW1000 wiring (`firmware/src/board.h`): CS 10, RST 9, IRQ 3, on SPI0
  (SCK 13, MOSI 11, MISO 12).
- The firmware uses no other pins. The Teensy LED shares pin 13 with SCK, so
  there is no status LED; use the console.
- SPI runs at 20 MHz (the DW1000 maximum); both tested boards pass `spitest`
  at every clock from 2 to 20 MHz. The firmware checks the bus at every radio
  reset and steps down through 16/12/8/6/4/2 MHz if a check fails.
- Settings live in the emulated EEPROM from byte 256 on, so whatever another
  firmware keeps in the low bytes survives.

## First run checklist

On the console (`tools/uwbcon.py /dev/ttyACMx info`, or any terminal):

1. `info`: `DEV_ID deca0130` means the DW1000 answers. `NOT READY` means it
   failed at every SPI clock (check power and wiring with `reg 0`).
2. `spitest`: SPI integrity from 2 to 20 MHz.
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
| `opt [preload\|ackpend\|dblbuf on\|off]` | throughput features, for experiments (not saved) |
| `reg <id> [sub] [len]` | read a DW1000 register (hex) |
| `reset` | reset and reconfigure the DW1000 |
| `reboot` / `bootloader` | restart / enter the Teensy bootloader |

Defaults: channel 5, PRF 64 MHz, 6.8 Mbit/s, preamble 128 (code 10), PAN
`5557`, peer learned automatically, 3 retries, `backoff 800` (pause when the
peer's ACK says it has data), `ipqueue 8`.

## Measured performance

Two boards on the same desk, both on one PC through `tools/nspair.sh`,
defaults throughout, RSSI about -67 dBm (2026-11-10).

| Test | Result |
|---|---|
| Radio only, `flood 10` (1012-byte frames) | 4.04 Mbit/s, ~0.2 % retransmissions |
| Radio only, `ping` 64 / 1012 bytes | 1.0 / 4.9 ms round trip, no loss |
| IP ping, idle | 1.8 ms |
| IP ping, 1011-byte packets | 6.4 ms, no loss |
| TCP (iperf3), A→B / B→A | 2.9-3.0 / 3.0-3.1 Mbit/s |
| Ping while TCP runs | ~20 ms |
| UDP at 3 Mbit/s | 0 % loss |
| UDP at 5 Mbit/s (overload) | 3.5 Mbit/s delivered, ping 49 ms |

Why this is about half of 6.8 Mbit/s, what was tried to get more (preloading
frames, double buffering, ACK-driven pauses), what worked and what didn't:
[docs/throughput.md](docs/throughput.md). In short:
- one-way traffic is limited by moving each 1 KB frame over SPI (450 µs at
  the DW1000's 20 MHz maximum);
- TCP is limited by its own ACKs, each of which costs a full radio exchange.

## Design notes

**Why the link acknowledges and retransmits even though TCP is reliable.**
TCP reads every lost segment as congestion and halves its sending rate, so
1-2 % random radio loss already costs most of the throughput. Also, UWB has
no real carrier sense, and with TCP both ends transmit (data one way, ACKs the
other), so the two radios collide: 3-6 % of frames in the measurements above.
A short local retry (hardware ACK about 170 us after the frame, at most 3
retries) hides that from TCP. Wi-Fi does the same for the same reason. For
UDP that would rather lose than wait, `set udparq 0` sends UDP without ACKs
while TCP keeps them, and `set retries 0` turns ACKs off completely.

**Channel access.** UWB has no real carrier sense. The firmware:
- defers while the chip reports an incoming preamble;
- uses the 802.15.4 "frame pending" bit in the hardware ACKs to take turns:
  a node with queued frames sets it in its ACKs, and the sender then pauses
  `backoff` µs for it.

A pause that is fixed and the same for both ends turned out to be fragile:
with some values both ends start together every time
([docs/throughput.md](docs/throughput.md)).

**Still unverified:** whether the DW1000's hardware ACK touches the TX buffer.
The firmware rewrites the first 16 bytes after each one, which is harmless
either way.

**Possible later improvements:**
- aggregating several small packets into one radio frame, which would help
  TCP most;
- a 64-symbol preamble;
- CDC-NCM, so no host daemon is needed;
- a systemd unit and udev rule for `uwbnetd`.

Double buffering was tried and doesn't work on this chip; see the throughput
doc.
