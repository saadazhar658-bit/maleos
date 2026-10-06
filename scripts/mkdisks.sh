#!/usr/bin/env bash
# Build the test disk images used by `make test` and the storage / filesystem self-tests.
#
#   build/disks/ide.img   8 MiB, ext2 (1 KiB blocks) in the first 6 MiB, scratch space after
#   build/disks/sata.img  8 MiB, ext2 (4 KiB blocks) in the first 6 MiB, scratch space after
#
# The kernel verifies file contents against a deterministic pattern, so nothing else
# needs to be stored alongside the images.
set -euo pipefail

OUT="${1:-build/disks}"
MKE2FS="$(command -v mke2fs || echo /sbin/mke2fs)"
[ -x "$MKE2FS" ] || { echo "error: mke2fs not found (install e2fsprogs)" >&2; exit 1; }
mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

python3 - "$WORK" <<'PY'
import os, sys

root = sys.argv[1]

def pattern(n):
    # Must match pattern_byte() in src/fs/fs_selftest.c
    return bytes(((i * 31 + (i >> 8)) & 0xFF) for i in range(n))

def write(path, data):
    full = os.path.join(root, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)

for disk, label in (("ide", "hda"), ("sata", "sda")):
    base = os.path.join(root, disk)
    os.makedirs(base)
    def w(p, d): write(os.path.join(disk, p), d)

    w("hello.txt", f"Hello from ext2 on {label}!\n".encode())
    w("empty", b"")
    w("dir/nested/deep.txt", b"deep file\n")
    w("big.bin", pattern(600000))          # 1 KiB blocks: needs double-indirect blocks
    w("medium.bin", pattern(70000))        # needs single-indirect blocks
    w("name_" + "x" * 190 + ".txt", b"long name\n")
    for i in range(200):                   # a directory that spans several blocks
        w(f"many/f{i:03d}", f"file {i}\n".encode())
    w(f"only_{disk}.txt", f"only on {label}\n".encode())

    sparse = os.path.join(base, "sparse.bin")
    with open(sparse, "wb") as f:
        f.write(b"HEAD")
        f.seek(500000); f.write(b"MIDDLE")
        f.seek(1048576 - 4); f.write(b"TAIL")

    os.symlink("hello.txt", os.path.join(base, "link"))
    os.symlink("./" * 45 + "hello.txt", os.path.join(base, "longlink"))   # > 60 bytes: stored in a block
    os.symlink("dir/nested", os.path.join(base, "dirlink"))
    os.symlink("nowhere", os.path.join(base, "dangling"))
PY

make_disk() {  # name  tree  blocksize  fs_blocks
    local img="$OUT/$1.img"
    rm -f "$img"
    truncate -s 8M "$img"
    "$MKE2FS" -q -F -t ext2 -b "$3" -L "$1" -d "$WORK/$2" "$img" "$4"
}

make_disk ide  ide  1024 6144    # 6 MiB filesystem, 2 MiB scratch
make_disk sata sata 4096 1536

echo "created $OUT/ide.img and $OUT/sata.img"
