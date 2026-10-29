# Vladiktron Spoke — the Model:Cycles OS on a Mac

Runs Elektron's own Model:Cycles OS 1.13 on a virtual board. The firmware comes from your
`model-cycles_OS1.13.syx`; nothing from it is bundled.

```sh
./build.sh ../model-cycles_OS1.13.syx     # builds everything, installs your firmware files
open "build/Vladiktron Spoke.app"
```

The build needs only CMake and the Xcode command line tools; everything else is in this repository, including
Unicorn (`vendor/`). `build.sh` builds Unicorn with CMake, the app with Swift Package Manager (`Package.swift`; open
it in Xcode to edit and debug), wraps it into a bundle and signs that with the hardened runtime and
`app/Spoke.entitlements` (JIT for Unicorn, audio input for USB audio). The signature is ad hoc unless
`SIGN_ID="Developer ID Application: …"` is set.

Without the `.syx` (or on a Mac where it was never installed), the app asks for it on first run: choose
`model-cycles_OS1.13.zip` as downloaded from [Elektron's support page](https://www.elektron.se/support-downloads/modelcycles#resources),
or the `.syx` inside it, or drop either on the window. Only Elektron's original OS 1.13 is accepted (version and
SHA-256 checked; other versions, modified OSes, the Model:Samples OS and damaged files each get their own message).
**Model:Cycles → Install OS…** does the same later, for example to restore `flash.factory.bin`.

Data lives in `~/Library/Application Support/Vladiktron Spoke/` (menu **Model:Cycles → Reveal Data Folder**): `main_os.bin`
(decoded OS), `flash.bin` (boot flash, holds settings), `flash.factory.bin` (the untouched flash image, for Factory
reset), `emmc.img` + `.extcsd` (the +Drive). `SPOKE_DATA=/some/folder` points the app at another folder. The app
used to be called Model Mac: on first launch it moves `ModelMac/` and its settings (bundle ID `local.modelmac.app`)
over, unless the new folder already exists.

**Quitting** powers the Model:Cycles off the way its power key does: the OS shows "Shutting down...", saves its
working state (the active project) to the +Drive and powers off, which takes about two seconds. Only then does the
app close the images. (If the OS has not powered off after 5 s, the CPU is stopped anyway.)

**Model:Cycles → Reset +Drive…** starts again with an empty +Drive, which the OS sets up at the next boot
(settings stay). **Factory reset…** does that and also puts back the factory flash image (settings). Both ask
first, and shut the OS down cleanly before they change any files.

**Presets** (FUNC + PRESET MENU) live in a file system on the +Drive that a real unit has formatted at the factory.
When the +Drive has none yet (a new image, or one made by an earlier version of this app), the board has the OS
format it at boot; projects already on the +Drive are kept. The **FACTORY** folder's categories (BD, CP, …, TO) stay
empty: the OS fills them from a block of preset data written to the +Drive at Elektron's factory, which is in no OS
update (checked in 1.12 and 1.13), so there is nothing to fill them from. Your own presets save and load normally.

## Pieces

| File | What |
|---|---|
| `mcboard.c/.h` | Virtual board: CPU (patched Unicorn), interrupt controllers, PIT and DMA timers, GPIO, SPI flash, eSDHC + eMMC (Samsung "4FTE4R" in pSLC mode), eDMA, front-panel bus, MIDI UART (UART9), USB device controller with a built-in USB host, audio-codec I2C stub |
| `app/` | SwiftUI app: runs the board on its own thread, draws the 128×64 screen, plays the audio through Core Audio (`SPOKE_AUDIOLOG=1` logs underruns, drops, queue depth and peak level every 5 s; `SPOKE_TEST="8:play,20:out=BuiltInSpeakerDevice,30:out=,60:quit"` presses PLAY, routes USB audio and quits on a schedule, for scripted runs), MIDI as CoreMIDI ports, MiniLab 3 mapping (`MiniLab.swift`) |
| `mcboot` | Headless runner: `./mcboot build/main_os.bin <flash> <emmc> [seconds] [screen.png]` |
| `mcaudio_test`, `mcmidi_test`, `mcusb_test` | Headless checks: record the OS's output while playing; MIDI notes in on each channel, and what comes out; USB link, USB-MIDI and USB audio both ways, sample for sample (PASS/FAIL) |
| `mcrt_test` | Headless real-time check (PASS/FAIL): `./mcrt_test build/main_os.bin <flash> <emmc> [seconds per phase]`. With the board on its own thread as in the app, it plays a pattern and reads the main output every 1 ms, with USB audio off, then with both streams on, then off again. Each phase fails if the output runs below 48 kHz, if a block arrives more than 10 ms late, or if a block comes out repeated (the same as the one or two before: a half of the OS's audio ring it had not rendered). Run it on an otherwise idle Mac. |
| Real-time log | `MCB_RTLOG=1` logs once a second how the board kept time: CPU slices, audio blocks (late, resynced, and what held them back), USB microframes and stalls, SOF interrupts, timer-2 reads, the host time the microframes took, and how long the CPU thread slept while the OS was idle. `MCB_NOIDLE=1` turns the idle sleep off (to compare). |
| eMMC log | eMMC errors (unanswered commands, accesses past the end of the card, failed image I/O) are always logged to stderr and counted in the status line. `MCB_EMMCLOG=1` logs every command. |
| Hang log | `MCB_HANGLOG=1` logs the OS's crash handler (its exception frame and registers) and, when the OS has not reached its idle loop for 2 s, where its CPU slices end. To catch it from the app: `open -n --env MCB_HANGLOG=1 --stderr <file> "build/Vladiktron Spoke.app"`. |
| `mcshutdown_test` | Headless check (PASS/FAIL): `./mcshutdown_test build/main_os.bin build/flash.bin`. On a fresh +Drive in a temporary folder, it changes the project, powers off with the power key, boots again and checks that the change survived, with no eMMC errors. A control run shows that a hard stop loses the change. |
| `mctiming_test` | Headless check (ok/FAIL): `./mctiming_test build/main_os.bin build/flash.bin`. On a fresh +Drive, past the 32 s at which DMA timer 0 first wraps: the timer keeps counting, knob events step DECAY as the MiniLab mapping expects (read from RAM), tap tempo sets 120 and 150 BPM, MIDI clock reaches the OS during a long SysEx on TRS and USB (the tempo it measures follows a change from 120 to 150 BPM), and the OS never enters its exception handler. About 70 s. |
| `mcunit_test` | Unit check of the board's own logic without the OS (ok/FAIL), so it needs no firmware: `./mcunit_test`. A flash.bin that is not 2 MB is refused, the USB-MIDI byte parser (running status, real-time bytes, SysEx endings), clock and start going ahead of a waiting SysEx on both ports, unmapped accesses stop the CPU, USB playback packets stay within their buffer. It includes `mcboard.c` to reach its internals. |
| `mcemac_test` | Checks the ColdFire multiply-accumulate (MAC) that the patched Unicorn translates into JIT code against the C helper it replaced, on random instructions in every mode, no firmware needed: `./mcemac_test [cases]`. |
| `mcfirmware_test` | The OS file decoder (ok/FAIL), with SysEx files made up in the test, so no firmware is needed: `./mcfirmware_test [model-cycles_OS1.13.syx]`. Each way a file can be wrong gets its own message (another device or version, a cut or damaged file, a modified OS); with the real file (by default `../model-cycles_OS1.13.syx`) it also checks that it decodes. |
| `swift test` | The MiniLab 3 mapping (`tests/swift`): PANEL keys, pads, faders, the main encoder, and knob and pitch-strip input replayed from recordings of the hardware (`tests/fixtures`); and the parser that splits the board's MIDI output into messages for the CoreMIDI sources. |
| `test.sh` | Runs `mcunit_test`, `mcemac_test`, `mcfirmware_test`, `swift test` and `mctiming_test` after `./build.sh`. GitHub Actions (`.github/workflows/ci.yml`) runs `build.sh` and `test.sh` on every push and pull request, without firmware (`mctiming_test` is skipped there). |
| `mcengine.c`, `mcbench` | The six-voice synth engine alone (bit-exact with Modded-Cycles' Python model) |
| `vendor/unicorn` | Unicorn 2.1.4 with this project's changes already applied (`vendor/README.md`, `vendor/unicorn-2.1.4.patch`) |
| `patch_unicorn.py` | How those changes are made to a stock Unicorn 2.1.4: fixes for ColdFire: EMAC maths, QEMU's own exceptions, IRQ lines, safe stops, an SR read that no longer changes the condition codes; speed: fast stores to RAM pages that hold no code, the JIT left executable for a whole run, no per-exit `isb`+register read in release builds |
| `mcfirmware.c/.h`, `mcinstall` | Decodes the `.syx` (SysEx packets and checksums, ELE3 container, aPLib-variant MAIN OS) and checks it is OS 1.13; a C port of `mtlib` (Modded-Cycles, MIT), used by the app and by `./mcinstall <syx> <dir>` |

## Playing it

- **Keys and pads**: click and hold. ⌥-click latches a key down (click again to release), for combinations
  such as PATTERN + step or TRACK + pad.
- **Knobs**: drag up/down, or scroll over them. One step is one detent. Click LEVEL/DATA without dragging to
  press it (open, confirm).
- **Keyboard**: Shift = FUNC, Space = PLAY, Return = LEVEL/DATA press.
- LEDs, the screen and all menus come from the OS itself.
- **MIDI**: two CoreMIDI port pairs, as the hardware shows up on a Mac: **"Model:Cycles"** is its USB-MIDI port,
  **"Model:Cycles TRS"** the MIDI IN and OUT/THRU jacks. Notes on channels 1–6 play tracks 1–6, the auto channel
  (10) the active track. What the Model:Cycles sends (clock, notes, CCs) is set in its own MIDI config menu, as
  on the hardware; out of the box it sends nothing. `MCB_MIDILOG=1` logs each byte received on the jacks.
- **USB**: the board is plugged into a built-in host, which enumerates the OS's USB device (audio + MIDI
  configuration) and carries USB-MIDI and USB audio; the bottom edge of the window shows the link.
  `MCB_USBLOG=1` logs enumeration and packets, `MCB_NOUSB=1` leaves the cable out.
- **USB audio** (menu **USB Audio**): as a USB audio interface the Model:Cycles sends its main mix (48 kHz,
  stereo) and plays what it is sent, mixed into its output at −18 dB. *Model:Cycles to* picks the Mac output that
  gets its stream, *Into Model:Cycles from* the input that feeds it (macOS asks for microphone access). With a
  loopback device such as BlackHole a DAW can record the Model:Cycles or play into it. Each stream runs only while
  routed; the emulator then works in 125 µs steps, one USB microframe. With nothing routed, USB audio costs the
  emulator about 0.15 ms of host time per second; the main output comes first either way.
- **Arturia MiniLab 3**, picked up automatically when connected. Put it in **DAW mode** (Shift + Prog) and it
  becomes the Model:Cycles' whole front panel, with pad colours following the Model:Cycles' LEDs and names on its
  display:

  | MiniLab 3 (DAW mode) | Model:Cycles |
  |---|---|
  | Shift | FUNC, held while Shift is held |
  | main encoder, its press | LEVEL/DATA, LEVEL/DATA press |
  | Shift + main encoder press | keys: PANEL (the default) ⇄ NOTES (keys play the active track, strips bend and modulate) |
  | knobs 1–8 (endless) | PITCH, DECAY, COLOR, SHAPE, SWEEP, CONTOUR, DELAY SEND, REVERB SEND |
  | faders 1–4 | LFO SPEED, VOLUME + DIST, SWING, CHANCE: the fader position is the value (sent as MIDI CC 102, 7, 15, 14 on the auto channel) |
  | pads A1–A6, A7, A8 | the six pads (velocity), PLAY, STOP |
  | pads B1–B8 (optional) | RECORD, PATTERN, TRACK, PAGE, RETRIG, LFO, BACK, MACHINES |
  | PANEL keys (octave 0) | F3–G#4 = trigs 1–16; C3–E3 = FUNC, RECORD, PATTERN, TRACK, PAGE; A4–C5 = RETRIG, PUNCH, GATE, BACK |
  | PANEL strips (tap a half) | pitch strip: TEMPO above, SETTINGS below; mod strip: MACHINES above, LFO below |

  Shift + a key gives its FUNC function, as printed under the Model:Cycles keys (Shift + BACK opens the pad
  configuration menu, Shift + PUNCH the QUANTIZE menu, and so on). Shift + pads 1–3 stay the MiniLab's own (pad
  bank, program), so FUNC + those pads goes through the FUNC key of the PANEL layer. Each value a knob moves is one value step on the Model:Cycles; turned fast, the
  Model:Cycles speeds up as with its own knobs.

  In its Arturia mode, with factory settings, the MiniLab plays the Model:Cycles with a smaller mapping: keys and
  strips play the active track, pads A1–A6 are the pads, A7 / A8 PLAY / STOP, B1–B6 select track 1–6, B7 / B8
  RECORD / TEMPO, knobs 1–8 (absolute: they stop at 0 and 127) and faders 1–4 as above. Other MiniLab
  programs (or controllers) need their CC numbers in `MiniLab.swift`.

## Licence

GPL-2.0-only (see `LICENSE` at the repository root); third-party code and its licences are listed in
`THIRD-PARTY-NOTICES.md`. Model:Cycles and Elektron are trademarks of Elektron Music Machines MAV AB; this project
is not affiliated with or endorsed by Elektron.

## Not done yet

See [RELEASE.md](RELEASE.md) for everything between this and a release.
