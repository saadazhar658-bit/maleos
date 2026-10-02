#!/usr/bin/env bash
# Build a reproducible x86_64-elf cross toolchain (binutils + GCC) into
# ./toolchain. Takes a while; only needed if you want a true cross compiler.
# The Makefile also works with the host GCC in freestanding mode.
#
# Usage: ./scripts/build-toolchain.sh
# Then:  export PATH="$PWD/toolchain/bin:$PATH"
set -euo pipefail

BINUTILS_VERSION="${BINUTILS_VERSION:-2.42}"
GCC_VERSION="${GCC_VERSION:-13.2.0}"
TARGET="x86_64-elf"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="$ROOT/toolchain"
SRC="$ROOT/toolchain-src"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$SRC" "$PREFIX"
export PATH="$PREFIX/bin:$PATH"

cd "$SRC"

if [ ! -d "binutils-$BINUTILS_VERSION" ]; then
    curl -fLO "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz"
    tar xf "binutils-$BINUTILS_VERSION.tar.xz"
fi
if [ ! -d "gcc-$GCC_VERSION" ]; then
    curl -fLO "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz"
    tar xf "gcc-$GCC_VERSION.tar.xz"
fi

echo ">> Building binutils $BINUTILS_VERSION"
mkdir -p build-binutils && cd build-binutils
"../binutils-$BINUTILS_VERSION/configure" --target="$TARGET" --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$JOBS"
make install
cd ..

echo ">> Building GCC $GCC_VERSION"
mkdir -p build-gcc && cd build-gcc
"../gcc-$GCC_VERSION/configure" --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j"$JOBS" all-gcc all-target-libgcc
make install-gcc install-target-libgcc

echo
echo "Done. Add the toolchain to your PATH:"
echo "  export PATH=\"$PREFIX/bin:\$PATH\""
