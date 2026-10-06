#!/usr/bin/env bash
# End-to-end keyboard test: boots the kernel (without the debug-exit device, so it keeps
# running), injects key presses through the QEMU monitor, and checks that the kernel's
# console thread received them through the real 8042 -> I/O APIC -> IRQ -> decoder path.
set -euo pipefail

ISO="${1:-build/maleos.iso}"
TIMEOUT="${TIMEOUT:-60}"
WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
SOCK="$WORK/mon.sock"
trap 'kill "$QPID" 2>/dev/null || true; rm -rf "$WORK"' EXIT

[ -f "$ISO" ] || { echo "error: ISO not found: $ISO (run 'make iso')" >&2; exit 2; }

qemu-system-x86_64 -cdrom "$ISO" -m 256M -display none -no-reboot \
    -serial "file:$LOG" -monitor "unix:$SOCK,server,nowait" &
QPID=$!

# Wait until the kernel has finished its self-tests and the console thread is running.
for _ in $(seq 1 "$TIMEOUT"); do
    grep -q "console: ready" "$LOG" 2>/dev/null && break
    sleep 1
done
grep -q "console: ready" "$LOG" || { echo "FAIL: kernel never reached the console"; cat "$LOG"; exit 1; }

python3 - "$SOCK" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.3)
s.recv(4096)
for key in ["h", "i", "shift-a", "1", "shift-1", "ret", "backspace", "ctrl-c", "up", "delete", "spc"]:
    s.send(f"sendkey {key}\n".encode())
    time.sleep(0.25)
s.recv(4096)
PY
sleep 1

echo "--- console output ---"
grep '\[console\]' "$LOG" || true
echo "----------------------"

FAILED=0
expect() {
    if grep -qF "$1" "$LOG"; then echo "PASS: $2"; else echo "FAIL: $2 (wanted: $1)" >&2; FAILED=1; fi
}
expect "key 0x68 'h'"  "plain key 'h'"
expect "key 0x69 'i'"  "plain key 'i'"
expect "key 0x41 'A'"  "shift+a gives 'A'"
expect "key 0x31 '1'"  "digit '1'"
expect "key 0x21 '!'"  "shift+1 gives '!'"
expect "key 0xa"       "enter"
expect "key 0x8"       "backspace"
expect "key 0x3"       "ctrl+c"
expect "key 0x101"     "up arrow"
expect "key 0x10a"     "delete"
expect "key 0x20 ' '"  "space"
exit $FAILED
