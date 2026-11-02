#!/bin/sh
# Build the Epson C33 cross toolchain (binutils 2.10.1 + gcc 3.3.2) for
# the WikiReader on a modern 64-bit Linux PC.  No Docker needed.
#
#   sh build-toolchain.sh /path/to/wikireader
#
# Installs into $WR/host-tools/toolchain-install, where the WikiReader
# makefiles and device/Makefile expect it.
#
# IMPORTANT: the compiler is built as a 32-bit program (gcc -m32).  Built
# as a 64-bit program, gcc 3.3 silently drops the top half of 64-bit
# constants (x * 0xc2b2ae3d27d4eb4f becomes x * 0x27d4eb4f), which
# breaks zstd and anything else using 64-bit maths.  The original
# WikiReader toolchain was built on 32-bit Linux, which hid this.
#
# Needs: gcc with 32-bit support (Debian/Ubuntu: gcc-multilib), make,
# bison, flex, patch, wget or curl, sha256sum.

set -e
WR=${1:-$HOME/wikireader}
HT="$WR/host-tools"
HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX="$HT/toolchain-install"
DL="$HT/toolchain-download"
SUMS="$HT/toolchain-sums"
JOBS=$(nproc 2>/dev/null || echo 4)

[ -d "$HT/toolchain-patches" ] || { echo "not a wikireader checkout: $WR"; exit 1; }
echo 'int main(void){return 0;}' > /tmp/m32test.c
gcc -m32 /tmp/m32test.c -o /tmp/m32test 2>/dev/null || {
	echo "gcc cannot build 32-bit programs; install gcc-multilib"; exit 1; }
for t in bison flex patch make; do
	command -v $t >/dev/null || { echo "missing: $t"; exit 1; }
done

mkdir -p "$DL"
fetch() {
	f="$DL/$1"
	if [ ! -f "$f" ]; then
		if command -v wget >/dev/null; then wget -O "$f" "$2"; else curl -L -o "$f" "$2"; fi
	fi
	(cd "$DL" && sha256sum --check "$SUMS/$1.SHA256")
}
fetch binutils-2.10.1.tar.gz https://ftp.gnu.org/gnu/binutils/binutils-2.10.1.tar.gz
fetch gcc-3.3.2.tar.gz https://ftp.gnu.org/gnu/gcc/gcc-3.3.2/gcc-3.3.2.tar.gz

CFLAGS_OLD="-O1 -g0 -std=gnu89 -fcommon -w -Wno-error -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion -Wno-incompatible-pointer-types -U_FORTIFY_SOURCE"
BUILD=i686-pc-linux-gnu
rm -rf "$PREFIX"
mkdir -p "$PREFIX"

echo "== binutils"
cd "$HT"
rm -rf binutils-2.10.1
tar -xzf "$DL/binutils-2.10.1.tar.gz"
cd binutils-2.10.1
for p in ../toolchain-patches/*-binutils-*.patch; do patch -p1 -s < "$p"; done
mkdir build && cd build
CC="gcc -m32" CFLAGS="$CFLAGS_OLD" ../configure --build=$BUILD --host=$BUILD \
	--prefix="$PREFIX" --target=c33-epson-elf --disable-nls > configure.log
make -j"$JOBS" MAKEINFO=true > make.log 2>&1 || { tail -20 make.log; exit 1; }
make install MAKEINFO=true > install.log

echo "== gcc"
cd "$HT"
rm -rf gcc-3.3.2
tar -xzf "$DL/gcc-3.3.2.tar.gz"
cd gcc-3.3.2
# the first patch also carries a prebuilt libgcc.a as a git binary diff,
# which patch(1) cannot apply; it is not needed (libgcc is built from
# source), so that one failure is expected, as in the original Makefile
for p in ../toolchain-patches/*-gcc-*.patch; do
	patch -p1 -s < "$p" || echo "(skipped binary part of $(basename "$p"))"
done
# fixes for modern bison and glibc (see the patch for details)
patch -p1 -s < "$HERE/gcc-3.3.2-modern-host.patch"
# make sure the parser is regenerated from the fixed grammar
rm -f gcc/c-parse.y gcc/c-parse.c
mkdir build && cd build
PATH="$PREFIX/bin:$PATH" CC="gcc -m32" CFLAGS="$CFLAGS_OLD" ../configure --build=$BUILD \
	--host=$BUILD --prefix="$PREFIX" --target=c33-epson-elf --enable-languages=c \
	--disable-nls > configure.log
PATH="$PREFIX/bin:$PATH" make -j"$JOBS" MAKEINFO=true CC="gcc -m32" CFLAGS="$CFLAGS_OLD" \
	> make.log 2>&1 || { tail -20 make.log; exit 1; }
PATH="$PREFIX/bin:$PATH" make install MAKEINFO=true > install.log

# check for the 64-bit constant bug
printf 'unsigned long long f(unsigned long long x){return x*0xc2b2ae3d27d4eb4full;}\n' > /tmp/c64.c
if "$PREFIX/bin/c33-epson-elf-gcc" -mc33pe -O1 -S -o - /tmp/c64.c | grep -q -- '-1028477379'; then
	echo "toolchain OK in $PREFIX"
else
	echo "WARNING: this gcc mangles 64-bit constants"; exit 1
fi
