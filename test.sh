#!/bin/sh
# Runs the quick checks after ./build.sh: the board's own logic without the OS (build/bin/mcunit_test), the patched
# Unicorn's multiply-accumulate against its old helper (build/bin/mcemac_test, a few seconds) and the OS file
# decoder with made-up files (build/bin/mcfirmware_test; it also checks model-cycles_OS1.13.syx if there), no firmware needed,
# the MiniLab 3 mapping (swift test, a few seconds) and the timing the OS
# sees past the first timer wrap (build/bin/mctiming_test, about a minute; needs build/main_os.bin and build/flash.bin from
# your .syx). The other mc*_test tools are run by hand (docs/DEVELOPMENT.md).
set -e
cd "$(dirname "$0")"
mkdir -p build
build/bin/mcunit_test
build/bin/mcemac_test
build/bin/mcfirmware_test
if swift test > build/swift-test.log 2>&1; then
    grep "Executed" build/swift-test.log | tail -1
else
    grep -E "error:" build/swift-test.log; echo "swift test FAILED (build/swift-test.log)"; exit 1
fi
if [ -f build/main_os.bin ] && [ -f build/flash.bin ]; then
    build/bin/mctiming_test build/main_os.bin build/flash.bin
else
    echo "no build/main_os.bin: skipping mctiming_test (run ./build.sh with your .syx)"
fi
