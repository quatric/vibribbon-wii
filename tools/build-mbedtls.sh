#!/bin/sh
# Cross-compiles mbedTLS (TLS 1.2 client only, see config/mbedtls_wii.h) into a
# static library for the Wii with devkitPPC. Output: third_party/wii/
set -eu

MBEDTLS_VER=3.6.7

: "${DEVKITPRO:=/opt/devkitpro}"
GCC=$DEVKITPRO/devkitPPC/bin/powerpc-eabi-gcc
AR=$DEVKITPRO/devkitPPC/bin/powerpc-eabi-ar

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/third_party/src
BUILD=$ROOT/third_party/build/mbedtls
PREFIX=$ROOT/third_party/wii
MBEDTLS_SHA256=$(cat "$ROOT/tools/mbedtls.sha256")

if [ -f "$PREFIX/lib/libmbedtls.a" ] && [ "${1:-}" != "--force" ]; then
	echo "mbedTLS already built in $PREFIX (use --force to rebuild)"
	exit 0
fi

mkdir -p "$SRC"
TARBALL=$SRC/mbedtls-$MBEDTLS_VER.tar.bz2
if [ ! -f "$TARBALL" ]; then
	curl -fsSL -o "$TARBALL.part" \
		"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER.tar.bz2"
	mv "$TARBALL.part" "$TARBALL"
fi
echo "$MBEDTLS_SHA256  $TARBALL" | shasum -a 256 -c -

rm -rf "$SRC/mbedtls-$MBEDTLS_VER" "$BUILD"
tar -xjf "$TARBALL" -C "$SRC"
MB=$SRC/mbedtls-$MBEDTLS_VER

mkdir -p "$BUILD" "$PREFIX/lib" "$PREFIX/include"
for src in "$MB"/library/*.c; do
	"$GCC" -Os -Wall -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float \
		-I"$DEVKITPRO/libogc/include" -I"$MB/include" -I"$MB/library" \
		-DMBEDTLS_CONFIG_FILE="\"$ROOT/config/mbedtls_wii.h\"" \
		-c "$src" -o "$BUILD/$(basename "${src%.c}").o"
done
rm -f "$PREFIX/lib/libmbedtls.a"
"$AR" rcs "$PREFIX/lib/libmbedtls.a" "$BUILD"/*.o
rm -rf "$PREFIX/include/mbedtls" "$PREFIX/include/psa"
cp -R "$MB/include/mbedtls" "$MB/include/psa" "$PREFIX/include/"
echo "mbedTLS $MBEDTLS_VER installed to $PREFIX"
