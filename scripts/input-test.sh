#!/usr/bin/env bash
# End-to-end keyboard test: boots the kernel (without the debug-exit device, so it keeps
# running), types a command into the shell through the QEMU monitor (real 8042 -> I/O APIC ->
# IRQ1 -> decoder -> console path) and checks the command ran.
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

# Wait until init has started the shell and it printed its prompt.
for _ in $(seq 1 "$TIMEOUT"); do
    grep -q '^maleos:.*\$ ' "$LOG" 2>/dev/null && break
    sleep 1
done
grep -q '^maleos:.*\$ ' "$LOG" || { echo "FAIL: shell never printed a prompt"; cat "$LOG"; exit 1; }

python3 - "$SOCK" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.3)
s.recv(4096)
# "echo Hx!" typed through the PS/2 keyboard (shift-h, x, shift-1), a typo fixed with
# backspace, then enter. Expected shell output: Hx!
keys = ["e", "c", "h", "o", "spc", "shift-h", "x", "z", "backspace", "shift-1", "ret"]
for key in keys:
    s.send(f"sendkey {key}\n".encode())
    time.sleep(0.25)
s.recv(4096)
PY
sleep 2

echo "--- serial output (tail) ---"
tail -n 8 "$LOG"
echo "----------------------------"

FAILED=0
expect() {
    if tr -d "\r" < "$LOG" | grep -qxF "$1"; then echo "PASS: $2"; else echo "FAIL: $2 (wanted line: $1)" >&2; FAILED=1; fi
}
expect 'Hx!' "keys 8042 -> IRQ1 -> decoder -> console -> sh -> echo (shift, backspace, enter)"
exit $FAILED
