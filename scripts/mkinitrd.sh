#!/usr/bin/env bash
# Pack initrd/ (plus a few generated test files) into a ustar archive.
#   usage: scripts/mkinitrd.sh [output.tar]
set -euo pipefail

OUT="${1:-build/initrd.tar}"
SRC="$(cd "$(dirname "$0")/.." && pwd)/initrd"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cp -a "$SRC/." "$WORK/"

python3 - "$WORK" <<'PY'
import os, sys
root = sys.argv[1]
# Must match pattern_byte() in src/fs/fs_selftest.c
open(os.path.join(root, "share", "pattern.bin"), "wb").write(
    bytes(((i * 31 + (i >> 8)) & 0xFF) for i in range(3000)))
os.symlink("hello.txt", os.path.join(root, "share", "hello.link"))
os.symlink("/etc/motd", os.path.join(root, "share", "motd.link"))
deep = os.path.join(root, "deep", *["level%02d_dir" % i for i in range(12)])  # path > 100 chars
os.makedirs(deep)
open(os.path.join(deep, "bottom.txt"), "w").write("found the bottom\n")
PY

mkdir -p "$(dirname "$OUT")"
tar --format=gnu --owner=0 --group=0 --numeric-owner --sort=name -C "$WORK" -cf "$OUT" .
echo "created $OUT ($(stat -c %s "$OUT") bytes)"
