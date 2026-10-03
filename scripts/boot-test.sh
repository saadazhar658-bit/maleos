#!/usr/bin/env bash
# Headless boot test: boots the ISO in QEMU and checks the serial output for
# every marker in MARKERS. Exits non-zero on failure.
#
# Environment:
#   MEM      guest RAM size            (default 256M)
#   TIMEOUT  seconds before giving up  (default 30)
#   MARKERS  '|'-separated list of strings that must appear
set -euo pipefail

ISO="${1:-build/maleos.iso}"
MEM="${MEM:-256M}"
TIMEOUT="${TIMEOUT:-30}"
MARKERS="${MARKERS:-MALEOS BOOT OK|MM SELFTEST: PASS|SCHED SELFTEST: PASS}"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

[ -f "$ISO" ] || { echo "error: ISO not found: $ISO (run 'make iso')" >&2; exit 2; }

# isa-debug-exit lets the guest terminate QEMU. The kernel writes 0x10, so a
# clean exit status is (0x10 << 1) | 1 = 33.
set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m "$MEM" \
    -display none \
    -serial "file:$LOG" \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -no-reboot
STATUS=$?
set -e

echo "--- serial output (RAM: $MEM) ---"
cat "$LOG"
echo "---------------------------------"

FAILED=0
IFS='|' read -ra LIST <<< "$MARKERS"
for m in "${LIST[@]}"; do
    if grep -qF "$m" "$LOG"; then
        echo "PASS: found '$m'"
    else
        echo "FAIL: missing '$m'" >&2
        FAILED=1
    fi
done

if [ "$STATUS" -ne 33 ]; then
    echo "FAIL: QEMU exit status $STATUS (expected 33: kernel did not finish cleanly)" >&2
    FAILED=1
fi

exit $FAILED
