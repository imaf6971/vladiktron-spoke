# Development

## Build

`./build.sh [path/to/model-cycles_OS1.13.syx]` needs only CMake and the Xcode command line tools; everything else is
in this repository, including Unicorn (`vendor/`). It builds Unicorn with CMake, the app with Swift Package Manager
(`Package.swift`; open it in Xcode to edit and debug), wraps it into a bundle and signs that with the hardened runtime
and `app/Resources/Spoke.entitlements` (JIT for Unicorn, audio input for USB audio). The signature is ad hoc unless
`SIGN_ID="Developer ID Application: …"` is set.

Everything built goes to `build/`, with tools and tests in `build/bin/`.

## Data folder

`~/Library/Application Support/Vladiktron Spoke/` holds `main_os.bin` (decoded OS), `flash.bin` (boot flash, holds
settings), `flash.factory.bin` (the untouched flash image, for Factory reset) and `emmc.img` + `.extcsd` (the +Drive).
`SPOKE_DATA=/some/folder` points the app at another folder. The app used to be called Model Mac: on first launch it
moves `ModelMac/` and its settings (bundle ID `local.modelmac.app`) over, unless the new folder already exists.

## Firmware install

The app checks the version and SHA-256 of the file; other versions, modified OSes, the Model:Samples OS and damaged
files each get their own message.

## Shutdown

Quitting powers the Model:Cycles off the way its power key does. Only after that does the app close the images. If
the OS has not powered off after 5 s, the CPU is stopped anyway. Reset +Drive and Factory reset ask first, and shut
the OS down cleanly before they change any files.

## Presets

Presets (FUNC + PRESET MENU) live in a file system on the +Drive that a real unit has formatted at the factory. When
the +Drive has none yet (a new image, or one made by an earlier version of this app), the board has the OS format it
at boot; projects already on the +Drive are kept. The FACTORY folder's categories (BD, CP, …, TO) stay empty: the OS
fills them from a block of preset data written to the +Drive at Elektron's factory, which is in no OS update (checked
in 1.12 and 1.13).

## USB

The board is plugged into a built-in host, which enumerates the OS's USB device (audio + MIDI configuration) and
carries USB-MIDI and USB audio; the bottom edge of the window shows the link. Each USB audio stream runs only while
routed; the emulator then works in 125 µs steps, one USB microframe. With nothing routed, USB audio costs the
emulator about 0.15 ms of host time per second; the main output comes first either way.

## Tests

`./test.sh` runs `mcunit_test`, `mcemac_test`, `mcfirmware_test`, `swift test` and `mctiming_test` after `./build.sh`.
GitHub Actions (`.github/workflows/ci.yml`) runs `build.sh` and `test.sh` on every push and pull request, without
firmware (`mctiming_test` is skipped there).

| Test | What |
|---|---|
| `mcunit_test` | Unit check of the board's own logic without the OS (ok/FAIL), so it needs no firmware: `build/bin/mcunit_test`. A flash.bin that is not 2 MB is refused, the USB-MIDI byte parser (running status, real-time bytes, SysEx endings), clock and start going ahead of a waiting SysEx on both ports, unmapped accesses stop the CPU, USB playback packets stay within their buffer. It includes `mcboard.c` to reach its internals. |
| `mcemac_test` | Checks the ColdFire multiply-accumulate (MAC) that the patched Unicorn translates into JIT code against the C helper it replaced, on random instructions in every mode, no firmware needed: `build/bin/mcemac_test [cases]`. |
| `mcfirmware_test` | The OS file decoder (ok/FAIL), with SysEx files made up in the test, so no firmware is needed: `build/bin/mcfirmware_test [model-cycles_OS1.13.syx]`. Each way a file can be wrong gets its own message (another device or version, a cut or damaged file, a modified OS); with the real file (by default `model-cycles_OS1.13.syx`) it also checks that it decodes. |
| `swift test` | The app's logic (`tests/swift`), no firmware needed. The MiniLab 3 mapping: PANEL keys, pads, faders, the main encoder, and knob and pitch-strip input replayed from recordings of the hardware (`tests/fixtures`). Also knob steps, the MIDI parser and port queues, audio output restarts, the board handle, and the problem and notice messages. |
| `mctiming_test` | Headless check (ok/FAIL): `build/bin/mctiming_test build/main_os.bin build/flash.bin`. On a fresh +Drive, past the 32 s at which DMA timer 0 first wraps: the timer keeps counting, knob events step DECAY as the MiniLab mapping expects (read from RAM), tap tempo sets 120 and 150 BPM, MIDI clock reaches the OS during a long SysEx on TRS and USB (the tempo it measures follows a change from 120 to 150 BPM), and the OS never enters its exception handler. About 70 s. |
| `mcshutdown_test` | Headless check (PASS/FAIL): `build/bin/mcshutdown_test build/main_os.bin build/flash.bin`. On a fresh +Drive in a temporary folder, it changes the project, powers off with the power key, boots again and checks that the change survived, with no eMMC errors. A control run shows that a hard stop loses the change. |
| `mcrt_test` | Headless real-time check (PASS/FAIL): `build/bin/mcrt_test build/main_os.bin <flash> <emmc> [seconds per phase]`. With the board on its own thread as in the app, it plays a pattern and reads the main output every 1 ms, with USB audio off, then with both streams on, then off again. Each phase fails if the output runs below 48 kHz, if a block arrives more than 10 ms late, or if a block comes out repeated (the same as the one or two before: a half of the OS's audio ring it had not rendered). It also prints energy, host cycles and wakeups per phase. Run it on an otherwise idle Mac. |
| `mcaudio_test`, `mcmidi_test`, `mcusb_test` | Headless checks: record the OS's output while playing; MIDI notes in on each channel, and what comes out; USB link, USB-MIDI and USB audio both ways, sample for sample (PASS/FAIL; `mcusb_test` runs against the wall clock and can fail under load, so rerun it). |
| `mcpanel_test` | Scripted front panel: `build/bin/mcpanel_test build/main_os.bin <flash> <emmc> <out_dir>` runs key, knob and pad actions and reports screen and LED changes. |

## Tools

| Tool | What |
|---|---|
| `mcboot` | Headless runner: `build/bin/mcboot build/main_os.bin <flash> <emmc> [seconds] [screen.png]` |
| `mckeyscan` | For each key: a fresh boot, one press, and the screen and LED changes saved: `build/bin/mckeyscan <os> <flash> <emmc> <out_dir>` |
| `mcinstall`, `src/mcfirmware.c/.h` | Decodes the `.syx` (SysEx packets and checksums, ELE3 container, aPLib-variant MAIN OS) and checks it is OS 1.13; a C port of `mtlib` (Modded-Cycles, MIT), used by the app and by `build/bin/mcinstall <syx> <dir>` |
| `tools/mcengine.c`, `mcbench` | The six-voice synth engine alone (bit-exact with Modded-Cycles' Python model) |
| `tools/scripts/patch_unicorn.py` | How the Unicorn changes are made to a stock 2.1.4: fixes for ColdFire: EMAC maths, QEMU's own exceptions, IRQ lines, safe stops, an SR read that no longer changes the condition codes, an instruction budget that ends slices; speed: fast stores to RAM pages that hold no code, the JIT left executable for a whole run, the multiply-accumulate as JIT code, no per-exit `isb`+register read in release builds |

`vendor/unicorn` is Unicorn 2.1.4 with these changes already applied (`vendor/README.md`,
`vendor/unicorn-2.1.4.patch`).

## The board

`src/mcboard.c/.h` holds the CPU (patched Unicorn), interrupt controllers, PIT and DMA timers, GPIO, SPI flash, eSDHC +
eMMC (Samsung "4FTE4R" in pSLC mode), eDMA, front-panel bus, MIDI UART (UART9), USB device controller with a built-in
USB host, and the audio codec's I2C registers (enough for MAIN VOLUME).

`app/` runs the board on its own thread, draws the 128×64 screen, plays the audio through Core Audio, exposes MIDI as
CoreMIDI ports and maps the MiniLab 3 (`MiniLab.swift`).

## Environment variables

| Variable | What |
|---|---|
| `SPOKE_DATA=/some/folder` | Use another data folder |
| `SPOKE_AUDIOLOG=1` | Log underruns, drops, queue depth and peak level every 5 s |
| `SPOKE_TEST="8:play,20:out=BuiltInSpeakerDevice,30:out=,60:quit"` | Press PLAY, route USB audio and quit on a schedule, for scripted runs |
| `SPOKE_SNAPSHOT=/path.png` | Save the window as a PNG 10 s after launch |
| `SPOKE_INSTALL=/path/to.syx` | Install this `.syx` or `.zip` 3 s after launch |
| `MCB_RTLOG=1` | Log once a second how the board kept time: CPU slices, audio blocks (late, resynced, and what held them back), USB microframes and stalls, SOF interrupts, timer-2 reads, the host time the microframes took, and how long the CPU thread slept while the OS was idle |
| `MCB_NOIDLE=1` | Turn the idle sleep off, to compare |
| `MCB_EMMCLOG=1` | Log every eMMC command. Errors (unanswered commands, accesses past the end of the card, failed image I/O) are always logged to stderr and counted in the status line |
| `MCB_HANGLOG=1` | Log the OS's crash handler (its exception frame and registers) and, when the OS has not reached its idle loop for 2 s, where its CPU slices end |
| `MCB_MIDILOG=1` | Log each byte received on the jacks |
| `MCB_CODECLOG=1` | Log the OS's writes to the codec registers |
| `MCB_USBLOG=1` | Log enumeration and packets |
| `MCB_NOUSB=1` | Leave the USB cable out |

To set one for the app:

```bash
open -n --env MCB_HANGLOG=1 --stderr hang.log "build/Vladiktron Spoke.app"
```
