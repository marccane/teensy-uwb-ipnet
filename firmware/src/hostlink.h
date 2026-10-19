#pragma once

#include <stddef.h>
#include <stdint.h>

// Framed packet interface on the first USB serial port (see docs/protocol.md).
//
// Each message is COBS(type, payload...) followed by a 0x00 delimiter:
//   0x01 DATA  one IP packet, either direction
//   0x02 CMD   host -> device, a console command line (ASCII)
//   0x03 RESP  device -> host, the command's output (ASCII)
namespace hostlink {

enum Type : uint8_t {
  kData = 0x01,
  kCmd = 0x02,
  kResp = 0x03,
};

// Reads from USB, queues IP packets for the radio and answers commands.
// Stops reading while the radio queue is full, which makes the host wait.
void poll();

// Passes an IP packet received over the air to the host. Dropped (and
// counted) if the port is closed or the host is not keeping up.
void deliver(const uint8_t* ip, size_t len);

}  // namespace hostlink
