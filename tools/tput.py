#!/usr/bin/env python3
"""Minimal TCP/UDP throughput test (for when iperf3 is not installed).

    tput.py server [--udp] [--port 5201]
    tput.py client 10.77.0.2 [--seconds 10] [--udp --rate 3e6 --size 983]

The server handles one test and exits. UDP datagrams carry a sequence number,
so the server reports loss and reordering.
"""

import argparse
import socket
import struct
import sys
import time


def report(label, nbytes, seconds, extra=""):
    rate = nbytes * 8 / seconds / 1e6 if seconds > 0 else 0.0
    print(f"{label}: {nbytes} bytes in {seconds:.2f} s = {rate:.3f} Mbit/s{extra}", flush=True)


def tcp_server(port):
    with socket.create_server(("0.0.0.0", port)) as srv:
        conn, peer = srv.accept()
        with conn:
            total, start = 0, None
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                if start is None:
                    start = time.monotonic()
                total += len(data)
            report(f"tcp from {peer[0]}", total, time.monotonic() - (start or time.monotonic()))


def tcp_client(host, port, seconds):
    payload = b"\xa5" * 65536
    with socket.create_connection((host, port), timeout=10) as s:
        total, start = 0, time.monotonic()
        while time.monotonic() - start < seconds:
            total += s.send(payload)
        s.shutdown(socket.SHUT_WR)
        s.recv(1)  # wait until the server has read everything and closed
        report("tcp sent", total, time.monotonic() - start)


def udp_server(port, idle):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("0.0.0.0", port))
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        s.settimeout(None)
        data, peer = s.recvfrom(65536)
        start = last = time.monotonic()
        total, count, highest, reordered = len(data), 1, struct.unpack("!I", data[:4])[0], 0
        s.settimeout(idle)
        while True:
            try:
                data, _ = s.recvfrom(65536)
            except socket.timeout:
                break
            last = time.monotonic()
            seq = struct.unpack("!I", data[:4])[0]
            if seq < highest:
                reordered += 1
            highest = max(highest, seq)
            total += len(data)
            count += 1
        sent = highest + 1
        lost = sent - count
        report(f"udp from {peer[0]}", total, last - start,
               f", {count}/{sent} datagrams, {lost} lost ({100 * lost / sent:.2f} %), "
               f"{reordered} reordered")


def udp_client(host, port, seconds, rate, size):
    interval = size * 8 / rate
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((host, port))
        pad = b"\x5a" * (size - 4)
        seq, start = 0, time.monotonic()
        next_send = start
        while True:
            now = time.monotonic()
            if now - start >= seconds:
                break
            if now < next_send:
                time.sleep(min(next_send - now, 0.002))
                continue
            try:
                s.send(struct.pack("!I", seq) + pad)
                seq += 1
            except OSError:
                pass  # ENOBUFS when the interface queue is full: counts as loss
            next_send += interval
        report("udp offered", seq * size, time.monotonic() - start, f", {seq} datagrams")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=["server", "client"])
    ap.add_argument("host", nargs="?")
    ap.add_argument("--port", type=int, default=5201)
    ap.add_argument("--udp", action="store_true")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--rate", type=float, default=3e6, help="UDP offered rate, bit/s")
    ap.add_argument("--size", type=int, default=983, help="UDP payload bytes (983 fills MTU 1011)")
    ap.add_argument("--idle", type=float, default=2.0, help="UDP server: stop after this idle time")
    args = ap.parse_args()

    if args.mode == "server":
        udp_server(args.port, args.idle) if args.udp else tcp_server(args.port)
    else:
        if not args.host:
            sys.exit("client needs a host")
        if args.udp:
            udp_client(args.host, args.port, args.seconds, args.rate, args.size)
        else:
            tcp_client(args.host, args.port, args.seconds)


if __name__ == "__main__":
    main()
