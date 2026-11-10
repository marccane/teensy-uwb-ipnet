#!/usr/bin/env bash
# Run both ends of a uwbnet link on one PC, without root.
#
# Each board gets its own unprivileged network namespace (user namespace +
# netns), so traffic between 10.77.0.1 and 10.77.0.2 has to cross the radio
# instead of being short-circuited by the local kernel.
#
#   tools/nspair.sh start /dev/ttyACM1 /dev/ttyACM2   # packet ports of A and B
#   tools/nspair.sh a ping 10.77.0.2                  # run a command as end A
#   tools/nspair.sh b iperf3 -s                       # ... or as end B
#   tools/nspair.sh status
#   tools/nspair.sh stop

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
daemon="$here/../host/uwbnetd"
state="${XDG_RUNTIME_DIR:-/tmp}/uwbnet-nspair"

ns_pid() { cat "$state/ns_$1.pid" 2>/dev/null; }

in_ns() {
  local pid
  pid=$(ns_pid "$1") || { echo "not running; use: $0 start <portA> <portB>" >&2; exit 1; }
  shift
  exec nsenter -t "$pid" -U -n --preserve-credentials "$@"
}

start_end() {  # name port address
  local name=$1 port=$2 addr=$3
  setsid unshare -rn sleep infinity < /dev/null > /dev/null 2>&1 &
  echo $! > "$state/ns_$name.pid"
  sleep 0.2
  local pid; pid=$(ns_pid "$name")
  nsenter -t "$pid" -U -n --preserve-credentials ip link set lo up
  setsid nsenter -t "$pid" -U -n --preserve-credentials \
    "$daemon" -v -a "$addr" "$port" < /dev/null > "$state/uwbnetd_$name.log" 2>&1 &
  echo $! > "$state/daemon_$name.pid"
}

stop_all() {
  local f
  for f in "$state"/daemon_*.pid "$state"/ns_*.pid; do
    [[ -e $f ]] || continue
    kill "$(cat "$f")" 2>/dev/null || true
    rm -f "$f"
  done
}

case "${1:-}" in
  start)
    [[ $# -eq 3 ]] || { echo "usage: $0 start <packet port A> <packet port B>" >&2; exit 2; }
    [[ -x $daemon ]] || { echo "build the daemon first: make -C host" >&2; exit 1; }
    mkdir -p "$state"
    stop_all
    start_end a "$2" 10.77.0.1/30
    start_end b "$3" 10.77.0.2/30
    sleep 1.5
    cat "$state"/uwbnetd_a.log "$state"/uwbnetd_b.log
    echo "A = 10.77.0.1 on $2, B = 10.77.0.2 on $3. Logs: $state/"
    ;;
  a | b)
    end=$1
    shift
    [[ $# -gt 0 ]] || set -- "${SHELL:-/bin/sh}"
    in_ns "$end" "$@"
    ;;
  status)
    for end in a b; do
      if pid=$(ns_pid $end) && kill -0 "$pid" 2>/dev/null; then
        echo "$end: $(nsenter -t "$pid" -U -n --preserve-credentials ip -br addr show uwb0 2>&1)"
        tail -1 "$state/uwbnetd_$end.log"
      else
        echo "$end: not running"
      fi
    done
    ;;
  stop)
    stop_all
    echo stopped
    ;;
  *)
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
