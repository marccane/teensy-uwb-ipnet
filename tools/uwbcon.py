#!/usr/bin/env python3
"""Run console commands on a uwbnet board and print the output.

    uwbcon.py /dev/ttyACM3 info stats
    uwbcon.py --listen 12 /dev/ttyACM3 "ping 10"   # keep reading for async output

Each command is sent as one line; output is read until the next "uwb> "
prompt. Standard library only.
"""

import argparse
import os
import select
import sys
import termios
import time
import tty

PROMPT = b"uwb> "


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.CLOCAL | termios.CREAD
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def read_for(fd, seconds, until=None):
    """Read for up to `seconds`, or until `until` appears in the data."""
    buf = b""
    end = time.monotonic() + seconds
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return buf
        ready, _, _ = select.select([fd], [], [], left)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        buf += chunk
        if until is not None and buf.endswith(until):
            return buf


def show(data):
    text = data.decode("utf-8", "replace").replace("\r\n", "\n")
    if text.endswith(PROMPT.decode()):
        text = text[: -len(PROMPT)]
    sys.stdout.write(text)
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port")
    ap.add_argument("commands", nargs="*")
    ap.add_argument("--timeout", type=float, default=10.0, help="per-command timeout, s")
    ap.add_argument("--listen", type=float, default=0.0,
                    help="keep printing output this many seconds after the last command")
    args = ap.parse_args()

    fd = open_port(args.port)
    try:
        # Opening raises DTR; the firmware answers with a banner and a prompt.
        read_for(fd, 1.0, until=PROMPT)
        for cmd in args.commands:
            os.write(fd, cmd.encode() + b"\r")
            out = read_for(fd, args.timeout, until=PROMPT)
            # Drop the echoed command line.
            out = out.split(b"\r\n", 1)[1] if b"\r\n" in out else out
            show(out)
        if args.listen:
            show(read_for(fd, args.listen))
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
