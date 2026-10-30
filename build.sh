#!/bin/sh
# Builds the Model:Cycles virtual board and the Mac app from this repository alone (nothing is downloaded).
#   ./build.sh [path/to/model-cycles_OS1.13.syx]
# Needs: CMake and the Xcode command line tools (clang, make, swift).
# Produces: build/bin/ with the command-line tools (mcinstall, mcbench, mcboot, mckeyscan) and tests (mc*_test)
#           and build/Vladiktron Spoke.app, signed with the hardened runtime: ad hoc, or with SIGN_ID="Developer ID
#           Application: ..." for distribution. If the .syx is there, also build/main_os.bin + build/flash.bin (for the
#           command-line tools), and installs the firmware into ~/Library/Application Support/Vladiktron Spoke as the
#           app's first-run screen would.
set -e
cd "$(dirname "$0")"
SYX=${1:-model-cycles_OS1.13.syx}
UC=vendor/unicorn
JOBS=$(sysctl -n hw.ncpu)
mkdir -p build/bin

# 1. Unicorn 2.1.4 with this project's ColdFire changes (vendor/README.md), m68k only, static. Built for the
#    app's oldest macOS (Package.swift, Info.plist), not the build machine's; a build/unicorn configured for
#    another one is configured again (and rebuilt).
MACOS_MIN=14.0
if [ ! -f build/unicorn/Makefile ] ||
   ! grep -q "^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]*=$MACOS_MIN\$" build/unicorn/CMakeCache.txt; then
    cmake -S $UC -B build/unicorn -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=m68k \
        -DUNICORN_BUILD_TESTS=OFF -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_MIN > build/unicorn-config.log
fi
cmake --build build/unicorn -j "$JOBS" > build/unicorn-build.log 2>&1

# 2. Firmware: decode your .syx (src/mcfirmware.c, as the app does)
cc -O2 -Wall -Wextra -Isrc src/mcfirmware.c tools/mcinstall.c -o build/bin/mcinstall
cc -O2 -Wall -Wextra -Isrc tests/c/mcfirmware_test.c -o build/bin/mcfirmware_test   # includes mcfirmware.c itself
if [ -f "$SYX" ]; then
    rm -rf build/fw
    build/bin/mcinstall "$SYX" build/fw
    cp build/fw/main_os.bin build/main_os.bin
    cp build/fw/flash.bin build/flash.bin
else
    echo "no $SYX: skipping the firmware (the app asks for it on first run)"
fi

# 3. Command-line tools
CFLAGS="-O2 -Wall -Wextra -Isrc -I$UC/include"
LIBS="build/unicorn/libunicorn.a -lz -lm -lpthread"
cc $CFLAGS tools/mcengine.c tools/mcbench.c $LIBS -o build/bin/mcbench
cc $CFLAGS src/mcboard.c tools/mcboot.c $LIBS -o build/bin/mcboot
cc $CFLAGS src/mcboard.c tools/mckeyscan.c $LIBS -o build/bin/mckeyscan
cc $CFLAGS src/mcboard.c tests/c/mcpanel_test.c $LIBS -o build/bin/mcpanel_test
cc $CFLAGS src/mcboard.c tests/c/mcaudio_test.c $LIBS -o build/bin/mcaudio_test
cc $CFLAGS src/mcboard.c tests/c/mcmidi_test.c $LIBS -o build/bin/mcmidi_test
cc $CFLAGS src/mcboard.c tests/c/mcusb_test.c $LIBS -o build/bin/mcusb_test
cc $CFLAGS src/mcboard.c tests/c/mcshutdown_test.c $LIBS -o build/bin/mcshutdown_test
cc $CFLAGS src/mcboard.c tests/c/mcrt_test.c $LIBS -o build/bin/mcrt_test
cc $CFLAGS src/mcboard.c tests/c/mctiming_test.c $LIBS -o build/bin/mctiming_test
cc $CFLAGS tests/c/mcunit_test.c $LIBS -o build/bin/mcunit_test   # includes mcboard.c itself
cc $CFLAGS tests/c/mcemac_test.c $LIBS -o build/bin/mcemac_test   # Unicorn alone: the EMAC multiply-accumulate

# 4. Mac app: the Swift package (Package.swift), wrapped into a bundle and signed with the hardened runtime and
#    app/Resources/Spoke.entitlements (JIT, audio input)
swift build -c release > build/app-build.log 2>&1 || { tail -30 build/app-build.log; exit 1; }
BIN=$(swift build -c release --show-bin-path)/VladiktronSpoke
rm -rf "build/Model Mac.app"                        # the app's old name
APP="build/Vladiktron Spoke.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp app/Resources/Info.plist "$APP/Contents/Info.plist"
cp LICENSE THIRD-PARTY-NOTICES.md "$APP/Contents/Resources/"
cp app/Resources/AppIcon.icns "$APP/Contents/Resources/"     # drawn by app/Resources/make_icon.swift
cp "$BIN" "$APP/Contents/MacOS/VladiktronSpoke"
TIMESTAMP=--timestamp=none                          # ad hoc; a Developer ID signature gets Apple's secure timestamp
[ -n "$SIGN_ID" ] && TIMESTAMP=--timestamp
codesign --force --options runtime $TIMESTAMP --entitlements app/Resources/Spoke.entitlements -s "${SIGN_ID:--}" "$APP"

# 5. Firmware files for the app (your own, never inside the bundle). Existing flash/eMMC state is kept;
#    flash.factory.bin is the untouched image the app's Factory reset goes back to.
DATA="$HOME/Library/Application Support/Vladiktron Spoke"
OLD="$HOME/Library/Application Support/ModelMac"     # the app's data under its old name moves over, as the app does
[ -d "$OLD" ] && [ ! -e "$DATA" ] && mv "$OLD" "$DATA"
[ -f "$SYX" ] && build/bin/mcinstall "$SYX" "$DATA"
echo "built build/bin/ and build/Vladiktron Spoke.app (data: $DATA)"
