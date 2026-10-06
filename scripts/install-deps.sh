#!/usr/bin/env bash
# Install the packages needed to build and run Maleos on Ubuntu / Debian.
set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
    build-essential nasm \
    bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo \
    qemu-system-x86 grub-pc-bin grub-common xorriso mtools e2fsprogs python3 \
    gdb clang-format
