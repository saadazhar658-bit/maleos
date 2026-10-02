#!/usr/bin/env bash
# Headless boot test: boots the ISO in QEMU and checks for the boot marker
# printed over the serial port. Exits non-zero on failure.
set -euo pipefail

ISO="${1:-build/maleos.iso}"
MARKER="${MARKER:-MALEOS BOOT OK}"
TIMEOUT="${TIMEOUT:-30}"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

[ -f "$ISO" ] || { echo "error: ISO not found: $ISO (run 'make iso')" >&2; exit 2; }

# isa-debug-exit lets the guest terminate QEMU. The kernel writes 0x10, so a
# clean exit status is (0x10 << 1) | 1 = 33.
set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 256M \
    -display none \
    -serial "file:$LOG" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -no-reboot
STATUS=$?
set -e

echo "--- serial output ---"
cat "$LOG"
echo "---------------------"

if grep -q "$MARKER" "$LOG"; then
    echo "PASS: boot marker found (qemu exit status $STATUS)"
    exit 0
fi

echo "FAIL: marker '$MARKER' not found (qemu exit status $STATUS)" >&2
exit 1
