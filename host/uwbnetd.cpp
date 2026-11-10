// uwbnetd: bridges a TUN interface to a uwbnet board's packet port.
//
//   uwbnetd [-i uwb0] [-a 10.77.0.1/30] [-m 1011] /dev/ttyACM1
//   uwbnetd -c "info" /dev/ttyACM1          run one console command and exit
//
// Protocol: docs/protocol.md. Needs CAP_NET_ADMIN (or an unprivileged network
// namespace, see README) to create the TUN interface.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cobs.h"

// <net/if.h> clashes with <linux/if.h>; this is the only function needed from it.
extern "C" unsigned int if_nametoindex(const char* ifname);

namespace {

constexpr uint8_t kTypeData = 0x01;
constexpr uint8_t kTypeCmd = 0x02;
constexpr uint8_t kTypeResp = 0x03;

constexpr size_t kMaxIp = 1011;  // firmware MTU
constexpr size_t kMaxMsg = 1 + 2048;  // largest RESP
constexpr size_t kMaxEncoded = cobs::maxEncodedLen(kMaxMsg) + 1;
// Serial write backlog. Kept small: the device pushes back when its radio
// queue is full, and anything buffered here only adds latency.
constexpr size_t kOutCap = 8192;
// The cdc_acm driver accepts up to 16 USB transfers before write() blocks,
// one per write() call, i.e. one per packet here. TIOCOUTQ reports them as
// 10 KB each (its transfer size). New packets are only taken from the TUN
// queue while at most one transfer is pending, so packets wait in the
// kernel's interface queue instead of in plain FIFOs further down.
constexpr int kMaxSerialBacklog = 16384;
constexpr int kBusyPollMs = 1;

volatile sig_atomic_t stopRequested = 0;
bool verbose = false;

void onSignal(int) { stopRequested = 1; }

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("uwbnetd: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

[[noreturn]] void die(const char* what) {
  logf("%s: %s", what, strerror(errno));
  exit(1);
}

int openSerial(const char* path) {
  const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) die(path);
  termios t;
  if (tcgetattr(fd, &t) < 0) die("tcgetattr");
  cfmakeraw(&t);
  t.c_cflag |= CLOCAL | CREAD;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSANOW, &t) < 0) die("tcsetattr");
  tcflush(fd, TCIOFLUSH);
  return fd;
}

// Sets the interface's transmit queue length over rtnetlink. The older
// SIOCSIFTXQLEN ioctl is refused inside unprivileged network namespaces.
void setTxQueueLen(const char* name, int qlen) {
  const unsigned index = if_nametoindex(name);
  if (!index) die("if_nametoindex");
  const int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (s < 0) die("netlink socket");
  struct {
    nlmsghdr hdr;
    ifinfomsg info;
    char attrs[RTA_SPACE(sizeof(uint32_t))];
  } req{};
  req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
  req.hdr.nlmsg_type = RTM_NEWLINK;
  req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  req.info.ifi_family = AF_UNSPEC;
  req.info.ifi_index = (int)index;
  auto* rta = (rtattr*)((char*)&req + NLMSG_ALIGN(req.hdr.nlmsg_len));
  rta->rta_type = IFLA_TXQLEN;
  rta->rta_len = RTA_LENGTH(sizeof(uint32_t));
  const uint32_t value = qlen;
  memcpy(RTA_DATA(rta), &value, sizeof(value));
  req.hdr.nlmsg_len = NLMSG_ALIGN(req.hdr.nlmsg_len) + RTA_ALIGN(rta->rta_len);
  if (send(s, &req, req.hdr.nlmsg_len, 0) < 0) die("netlink send");
  char reply[256];
  const ssize_t n = recv(s, reply, sizeof(reply), 0);
  if (n < 0) die("netlink recv");
  const auto* h = (const nlmsghdr*)reply;
  if (n >= (ssize_t)NLMSG_LENGTH(sizeof(nlmsgerr)) && h->nlmsg_type == NLMSG_ERROR) {
    const auto* err = (const nlmsgerr*)NLMSG_DATA(h);
    if (err->error) {
      errno = -err->error;
      die("setting txqueuelen");
    }
  }
  close(s);
}

int openTun(char* name, size_t mtu, int txQueueLen) {
  const int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) die("/dev/net/tun");
  ifreq ifr{};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
  if (ioctl(fd, TUNSETIFF, &ifr) < 0) die("TUNSETIFF");
  snprintf(name, IFNAMSIZ, "%s", ifr.ifr_name);

  const int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) die("socket");
  ifr.ifr_mtu = (int)mtu;
  if (ioctl(s, SIOCSIFMTU, &ifr) < 0) die("SIOCSIFMTU");
  close(s);
  // TUN keeps its own FIFO of txqueuelen packets (default 500, ~1 s at radio
  // speed) and drops only when it is full. A short queue keeps latency low.
  setTxQueueLen(name, txQueueLen);
  return fd;
}

// Assigns an IPv4 address/prefix and brings the interface up.
void configureInterface(const char* name, const char* cidr) {
  const int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) die("socket");
  ifreq ifr{};
  snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
  if (cidr) {
    char addr[64];
    snprintf(addr, sizeof(addr), "%s", cidr);
    int prefix = 32;
    if (char* slash = strchr(addr, '/')) {
      *slash = '\0';
      prefix = atoi(slash + 1);
    }
    auto* sin = (sockaddr_in*)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr, &sin->sin_addr) != 1 || prefix < 0 || prefix > 32) {
      logf("bad address '%s'", cidr);
      exit(2);
    }
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) die("SIOCSIFADDR");
    sin->sin_addr.s_addr = htonl(prefix ? 0xFFFFFFFFu << (32 - prefix) : 0);
    if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0) die("SIOCSIFNETMASK");
  }
  if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) die("SIOCGIFFLAGS");
  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) die("SIOCSIFFLAGS");
  close(s);
}

// Bytes waiting to be written to the serial port.
struct OutBuffer {
  uint8_t data[kOutCap];
  size_t head = 0;  // first unwritten byte
  size_t len = 0;

  size_t free() const { return kOutCap - head - len; }

  bool append(uint8_t type, const uint8_t* payload, size_t n) {
    uint8_t raw[kMaxMsg];
    if (n + 1 > sizeof(raw)) return false;
    if (head && free() < kMaxEncoded) {
      memmove(data, data + head, len);
      head = 0;
    }
    if (free() < cobs::maxEncodedLen(n + 1) + 1) return false;
    raw[0] = type;
    memcpy(raw + 1, payload, n);
    uint8_t* out = data + head + len;
    size_t enc = cobs::encode(raw, n + 1, out);
    out[enc++] = 0;
    len += enc;
    return true;
  }

  // Returns false on a fatal write error.
  bool flush(int fd) {
    while (len) {
      const ssize_t w = write(fd, data + head, len);
      if (w < 0) return errno == EAGAIN || errno == EINTR;
      head += w;
      len -= w;
    }
    head = 0;
    return true;
  }
};

// Splits the serial byte stream into decoded messages.
struct Decoder {
  uint8_t enc[kMaxEncoded];
  size_t encLen = 0;
  bool discarding = false;

  template <typename Fn>
  void feed(const uint8_t* p, size_t n, Fn&& onMessage) {
    for (size_t i = 0; i < n; i++) {
      if (p[i] != 0) {
        if (discarding) continue;
        if (encLen < sizeof(enc)) {
          enc[encLen++] = p[i];
        } else {
          discarding = true;
        }
        continue;
      }
      const size_t len = encLen;
      encLen = 0;
      if (discarding) {
        discarding = false;
        continue;
      }
      if (!len) continue;
      uint8_t msg[kMaxMsg];
      size_t msgLen;
      if (cobs::decode(enc, len, msg, sizeof(msg), &msgLen) && msgLen) onMessage(msg, msgLen);
    }
  }
};

struct Counters {
  uint64_t toRadio = 0, toRadioBytes = 0, fromRadio = 0, fromRadioBytes = 0;
  uint64_t tunDrops = 0, oversize = 0;
  int maxOutq = 0;  // largest backlog seen in the kernel's serial driver
};

int runCommand(int serial, const char* command) {
  OutBuffer out;
  const uint8_t sync = 0;
  if (write(serial, &sync, 1) < 0 && errno != EAGAIN) die("write");
  out.append(kTypeCmd, (const uint8_t*)command, strlen(command));
  Decoder dec;
  bool done = false;
  const time_t deadline = time(nullptr) + 30;
  while (!done && !stopRequested && time(nullptr) < deadline) {
    if (!out.flush(serial)) die("write");
    pollfd p{serial, (short)(POLLIN | (out.len ? POLLOUT : 0)), 0};
    if (poll(&p, 1, 1000) < 0 && errno != EINTR) die("poll");
    uint8_t buf[4096];
    const ssize_t n = read(serial, buf, sizeof(buf));
    if (n < 0 && errno != EAGAIN && errno != EINTR) die("read");
    if (n > 0) {
      dec.feed(buf, n, [&](const uint8_t* msg, size_t len) {
        if (msg[0] != kTypeResp) return;  // DATA from the air: ignore here
        fwrite(msg + 1, 1, len - 1, stdout);
        done = true;
      });
    }
  }
  if (!done) {
    logf("no response");
    return 1;
  }
  return 0;
}

int runBridge(int serial, int tun, const char* ifname) {
  OutBuffer out;
  Decoder dec;
  Counters c;
  // Ends any half-sent message from a previous session.
  const uint8_t sync = 0;
  if (write(serial, &sync, 1) < 0 && errno != EAGAIN) die("write");

  logf("%s up, bridging to the radio", ifname);
  time_t lastReport = time(nullptr);
  while (!stopRequested) {
    // Only take a packet from the kernel once the previous one is on its way
    // and the USB driver is nearly idle. The driver gives no wakeup when its
    // backlog drains, so check again shortly while it is busy.
    int outq = 0;
    if (ioctl(serial, TIOCOUTQ, &outq) < 0) outq = 0;
    if (outq > c.maxOutq) c.maxOutq = outq;
    const bool serialBusy = outq > kMaxSerialBacklog;
    const bool room = out.len == 0 && !serialBusy;
    pollfd fds[2];
    fds[0] = {serial, (short)(POLLIN | (out.len ? POLLOUT : 0)), 0};
    fds[1] = {tun, (short)(room ? POLLIN : 0), 0};
    if (poll(fds, 2, serialBusy ? kBusyPollMs : 1000) < 0) {
      if (errno == EINTR) continue;
      die("poll");
    }
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      logf("serial port closed");
      return 1;
    }
    if (fds[0].revents & POLLIN) {
      uint8_t buf[8192];
      const ssize_t n = read(serial, buf, sizeof(buf));
      if (n < 0 && errno != EAGAIN && errno != EINTR) die("serial read");
      if (n > 0) {
        dec.feed(buf, n, [&](const uint8_t* msg, size_t len) {
          if (msg[0] == kTypeData && len > 1) {
            if (write(tun, msg + 1, len - 1) < 0) {
              c.tunDrops++;
            } else {
              c.fromRadio++;
              c.fromRadioBytes += len - 1;
            }
          } else if (msg[0] == kTypeResp && verbose) {
            fwrite(msg + 1, 1, len - 1, stderr);
          }
        });
      }
    }
    if ((fds[1].revents & POLLIN) && room) {
      uint8_t pkt[2048];
      const ssize_t n = read(tun, pkt, sizeof(pkt));
      if (n < 0 && errno != EAGAIN && errno != EINTR) die("tun read");
      if (n > (ssize_t)kMaxIp) {
        c.oversize++;
      } else if (n > 0 && out.append(kTypeData, pkt, n)) {
        c.toRadio++;
        c.toRadioBytes += n;
      }
    }
    if (out.len && !out.flush(serial)) die("serial write");

    if (verbose && time(nullptr) - lastReport >= 10) {
      lastReport = time(nullptr);
      logf("to radio %llu pkts/%llu B, from radio %llu pkts/%llu B, tun drops %llu, oversize %llu, "
           "max serial backlog %d B",
           (unsigned long long)c.toRadio, (unsigned long long)c.toRadioBytes,
           (unsigned long long)c.fromRadio, (unsigned long long)c.fromRadioBytes,
           (unsigned long long)c.tunDrops, (unsigned long long)c.oversize, c.maxOutq);
    }
  }
  logf("stopping");
  return 0;
}

void usage() {
  fprintf(stderr,
          "usage: uwbnetd [-v] [-i ifname] [-a addr/prefix] [-m mtu] [-q len] <serial port>\n"
          "       uwbnetd -c <command> <serial port>\n"
          "  -i  interface name (default uwb0)\n"
          "  -a  IPv4 address to assign, e.g. 10.77.0.1/30 (default: leave unconfigured,\n"
          "      only bring the interface up)\n"
          "  -m  MTU, at most %zu (default %zu)\n"
          "  -q  interface transmit queue length, packets (default 8)\n"
          "  -c  send one console command over the packet port, print the reply, exit\n"
          "  -v  print counters every 10 s\n",
          kMaxIp, kMaxIp);
}

}  // namespace

int main(int argc, char** argv) {
  char ifname[IFNAMSIZ] = "uwb0";
  const char* address = nullptr;
  const char* command = nullptr;
  size_t mtu = kMaxIp;
  int txQueueLen = 8;
  int opt;
  while ((opt = getopt(argc, argv, "i:a:m:q:c:vh")) != -1) {
    switch (opt) {
      case 'i':
        snprintf(ifname, sizeof(ifname), "%s", optarg);
        break;
      case 'a':
        address = optarg;
        break;
      case 'm':
        mtu = strtoul(optarg, nullptr, 10);
        break;
      case 'q':
        txQueueLen = atoi(optarg);
        break;
      case 'c':
        command = optarg;
        break;
      case 'v':
        verbose = true;
        break;
      default:
        usage();
        return opt == 'h' ? 0 : 2;
    }
  }
  if (optind != argc - 1 || mtu < 68 || mtu > kMaxIp || txQueueLen < 1) {
    usage();
    return 2;
  }

  struct sigaction sa{};
  sa.sa_handler = onSignal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  const int serial = openSerial(argv[optind]);
  if (command) return runCommand(serial, command);

  const int tun = openTun(ifname, mtu, txQueueLen);
  configureInterface(ifname, address);
  return runBridge(serial, tun, ifname);
}
