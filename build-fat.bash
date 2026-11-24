#!/bin/bash
# Builds MacShell for 68k and PowerPC and combines them into one "fat"
# application: Power Macs run the native PowerPC code, 68k Macs the 68k code.
#
# The two Retro68 toolchains can be separate builds:
#   RETRO68_68K  Retro68 build folder with the 68k toolchain  (default ~/Retro68-build)
#   RETRO68_PPC  Retro68 build folder with the PowerPC toolchain (default ~/Retro68-build-ppc)
# Result: build-fat/MacShell.bin (MacBinary)
set -e

RETRO68_68K=${RETRO68_68K:-$HOME/Retro68-build}
RETRO68_PPC=${RETRO68_PPC:-$HOME/Retro68-build-ppc}
HERE=$(cd "$(dirname "$0")" && pwd)

BUILD_PARALLEL=
if command -v nproc >/dev/null 2>&1; then
  BUILD_PARALLEL="--parallel $(nproc)"
fi

if [ "$1" != "--combine-only" ]; then
echo "building PowerPC..."
rm -rf build-ppc
cmake -S . -B build-ppc -DCMAKE_TOOLCHAIN_FILE=$RETRO68_PPC/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build build-ppc $BUILD_PARALLEL

echo "building 68k..."
rm -rf build-m68k
cmake -S . -B build-m68k -DCMAKE_TOOLCHAIN_FILE=$RETRO68_68K/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build build-m68k $BUILD_PARALLEL

fi

echo "combining them..."
rm -rf build-fat
mkdir build-fat
# the 68k code, the PowerPC code (data fork) with the cfrg resource that
# points to it, and MacShell's resources (icons, dialogs, manual pages...)
$RETRO68_68K/toolchain/bin/Rez \
  $RETRO68_PPC/toolchain/RIncludes/RetroPPCAPPL.r \
  -I$RETRO68_PPC/toolchain/RIncludes -I$RETRO68_68K/toolchain/RIncludes \
  -DCFRAG_NAME="\"MacShell\"" \
  --copy build-m68k/MacShell.code.bin \
  -o build-fat/MacShell.bin \
  -t APPL -c MShl \
  --data build-ppc/MacShell.pef \
  build-m68k/resources.r.rsrc.bin build-m68k/symbolfont.r.rsrc.bin build-m68k/man.r.rsrc.bin

python3 "$HERE/tools/scrub_build_paths.py" build-fat/MacShell.bin
python3 "$HERE/tools/set_bundle_bit.py" build-fat/MacShell.bin
echo "done: build-fat/MacShell.bin"
