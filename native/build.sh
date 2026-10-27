#!/bin/sh
# Builds the Model:Cycles virtual board and the Mac app from scratch.
#   ./build.sh [path/to/model-cycles_OS1.13.syx]
# Needs: git, python3, cmake, ninja, pkg-config, Xcode command line tools.
# Produces: ./mcinstall, ./mcbench, ./mcboot, ./mcaudio_test, ./mcmidi_test, ./mcusb_test, ./mcshutdown_test, ./mcrt_test
#           and build/Vladiktron Spoke.app. If the .syx is there, also build/main_os.bin + build/flash.bin (for the
#           command-line tools), and installs the firmware into ~/Library/Application Support/Vladiktron Spoke as the
#           app's first-run screen would.
set -e
cd "$(dirname "$0")"
SYX=${1:-../model-cycles_OS1.13.syx}
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

# 2. Firmware: decode your .syx (mcfirmware.c, as the app does)
cc -O2 -Wall -Wextra mcfirmware.c mcinstall.c -o mcinstall
if [ -f "$SYX" ]; then
    rm -rf build/fw
    ./mcinstall "$SYX" build/fw
    cp build/fw/main_os.bin build/main_os.bin
    cp build/fw/flash.bin build/flash.bin
else
    echo "no $SYX: skipping the firmware (the app asks for it on first run)"
fi

# 3. Command-line tools
CFLAGS="-O2 -Wall -Wextra -I$UC/include"
LIBS="$UC/build/libunicorn.a -lz -lm -lpthread"
cc $CFLAGS mcengine.c mcbench.c $LIBS -o mcbench
cc $CFLAGS mcboard.c mcboot.c $LIBS -o mcboot
cc $CFLAGS mcboard.c mcaudio_test.c $LIBS -o mcaudio_test
cc $CFLAGS mcboard.c mcmidi_test.c $LIBS -o mcmidi_test
cc $CFLAGS mcboard.c mcusb_test.c $LIBS -o mcusb_test
cc $CFLAGS mcboard.c mcshutdown_test.c $LIBS -o mcshutdown_test
cc $CFLAGS mcboard.c mcrt_test.c $LIBS -o mcrt_test

# 4. Mac app
cc $CFLAGS -c mcboard.c -o build/obj/mcboard.o
cc $CFLAGS -c mcfirmware.c -o build/obj/mcfirmware.o
rm -rf "build/Model Mac.app"                        # the app's old name
APP="build/Vladiktron Spoke.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp app/Info.plist "$APP/Contents/Info.plist"
cp ../LICENSE ../THIRD-PARTY-NOTICES.md "$APP/Contents/Resources/"
swiftc -O -parse-as-library -import-objc-header app/Bridging.h app/*.swift build/obj/mcboard.o build/obj/mcfirmware.o \
    $UC/build/libunicorn.a -lz -o "$APP/Contents/MacOS/VladiktronSpoke"
codesign --force -s - "$APP" 2> /dev/null

# 5. Firmware files for the app (your own, never inside the bundle). Existing flash/eMMC state is kept;
#    flash.factory.bin is the untouched image the app's Factory reset goes back to.
DATA="$HOME/Library/Application Support/Vladiktron Spoke"
OLD="$HOME/Library/Application Support/ModelMac"     # the app's data under its old name moves over, as the app does
[ -d "$OLD" ] && [ ! -e "$DATA" ] && mv "$OLD" "$DATA"
[ -f "$SYX" ] && ./mcinstall "$SYX" "$DATA"
echo "built ./mcbench, ./mcboot and build/Vladiktron Spoke.app (data: $DATA)"
