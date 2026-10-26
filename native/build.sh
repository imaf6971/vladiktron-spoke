#!/bin/sh
# Builds the Model:Cycles virtual board and the Mac app from scratch.
#   ./build.sh [path/to/model-cycles_OS1.13.syx]
# Needs: git, python3, cmake, ninja, pkg-config, Xcode command line tools.
# Produces: build/main_os.bin + build/flash.bin (decoded from your .syx), ./mcbench, ./mcboot, ./mcaudio_test, ./mcmidi_test, ./mcusb_test, ./mcshutdown_test,
#           build/Model Mac.app, and installs the firmware files into ~/Library/Application Support/ModelMac.
set -e
cd "$(dirname "$0")"
SYX=${1:-../model-cycles_OS1.13.syx}
MODDED_CYCLES_REV=259441967e39b283e458e5027eda27c86a663562
UC=third_party/unicorn
mkdir -p third_party build/obj

# 1. Unicorn 2.1.4, patched for ColdFire (EMAC, exceptions, IRQ lines, safe stops)
if [ ! -d $UC ]; then
    git clone -q --depth 1 --branch 2.1.4 https://github.com/unicorn-engine/unicorn.git $UC
fi
python3 patch_unicorn.py $UC > /dev/null
if [ ! -f $UC/build/build.ninja ]; then
    cmake -S $UC -B $UC/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=m68k -DUNICORN_BUILD_TESTS=OFF \
        > build/unicorn-config.log
fi
ninja -C $UC/build > build/unicorn-build.log

# 2. Firmware: decode your .syx (Modded-Cycles' stdlib-only mtlib)
if [ ! -d third_party/Modded-Cycles ]; then
    git clone -q https://github.com/18nelli18/Modded-Cycles.git third_party/Modded-Cycles
    git -C third_party/Modded-Cycles checkout -q $MODDED_CYCLES_REV
fi
python3 -I extract_os.py "$SYX" third_party/Modded-Cycles/tools build/sections build/flash.bin
cp build/sections/section_3.bin build/main_os.bin

# 3. Command-line tools
CFLAGS="-O2 -Wall -Wextra -I$UC/include"
LIBS="$UC/build/libunicorn.a -lz -lm -lpthread"
cc $CFLAGS mcengine.c mcbench.c $LIBS -o mcbench
cc $CFLAGS mcboard.c mcboot.c $LIBS -o mcboot
cc $CFLAGS mcboard.c mcaudio_test.c $LIBS -o mcaudio_test
cc $CFLAGS mcboard.c mcmidi_test.c $LIBS -o mcmidi_test
cc $CFLAGS mcboard.c mcusb_test.c $LIBS -o mcusb_test
cc $CFLAGS mcboard.c mcshutdown_test.c $LIBS -o mcshutdown_test

# 4. Mac app
cc $CFLAGS -c mcboard.c -o build/obj/mcboard.o
APP="build/Model Mac.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp app/Info.plist "$APP/Contents/Info.plist"
swiftc -O -parse-as-library -import-objc-header mcboard.h app/*.swift build/obj/mcboard.o \
    $UC/build/libunicorn.a -lz -o "$APP/Contents/MacOS/ModelMac"
codesign --force -s - "$APP" 2> /dev/null

# 5. Firmware files for the app (your own, never inside the bundle). Existing flash/eMMC state is kept;
#    flash.factory.bin is the untouched image the app's Factory reset goes back to.
DATA="$HOME/Library/Application Support/ModelMac"
mkdir -p "$DATA"
cp build/main_os.bin "$DATA/main_os.bin"
cp build/flash.bin "$DATA/flash.factory.bin"
[ -f "$DATA/flash.bin" ] || cp build/flash.bin "$DATA/flash.bin"
echo "built ./mcbench, ./mcboot and build/Model Mac.app (data: $DATA)"
