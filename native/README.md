# Model Mac — Model:Cycles on a Mac

Runs Elektron's own Model:Cycles OS 1.13 on a virtual board. The firmware comes from your
`model-cycles_OS1.13.syx`; nothing from it is bundled.

```sh
./build.sh ../model-cycles_OS1.13.syx     # builds everything, installs your firmware files
open "build/Model Mac.app"
```

Data lives in `~/Library/Application Support/ModelMac/`: `main_os.bin` (decoded OS), `flash.bin` (boot
flash, holds settings), `emmc.img` + `.extcsd` (the +Drive; delete both to start from a fresh drive).

## Pieces

| File | What |
|---|---|
| `mcboard.c/.h` | Virtual board: CPU (patched Unicorn), interrupt controllers, PIT and DMA timers, GPIO, SPI flash, eSDHC + eMMC (Samsung "4FTE4R" in pSLC mode), eDMA, front-panel bus, MIDI UART (UART9), USB device controller with a built-in USB host, audio-codec I2C stub |
| `app/` | SwiftUI app: runs the board on its own thread, draws the 128×64 screen, plays the audio through Core Audio (`MODELMAC_AUDIOLOG=1` logs underruns), MIDI as CoreMIDI ports, MiniLab 3 mapping (`MiniLab.swift`) |
| `mcboot` | Headless runner: `./mcboot build/main_os.bin <flash> <emmc> [seconds] [screen.png]` |
| `mcaudio_test`, `mcmidi_test`, `mcusb_test` | Headless checks: record the OS's output while playing; MIDI notes in on each channel, and what comes out; USB link, USB-MIDI and USB audio both ways, sample for sample (PASS/FAIL) |
| `mcengine.c`, `mcbench` | The six-voice synth engine alone (bit-exact with Modded-Cycles' Python model) |
| `patch_unicorn.py` | Unicorn 2.1.4 fixes for ColdFire: EMAC maths, QEMU's own exceptions, IRQ lines, safe stops |
| `extract_os.py` | Decodes the `.syx` with Modded-Cycles' `mtlib` |

## Playing it

- **Keys and pads**: click and hold. ⌥-click latches a key down (click again to release), for combinations
  such as PATTERN + step or TRACK + pad.
- **Knobs**: drag up/down, or scroll over them. One step is one detent.
- **Keyboard**: Shift = FUNC, Space = PLAY.
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
  routed; the emulator then works in 125 µs steps, one USB microframe.
- **Arturia MiniLab 3** (factory settings), picked up automatically when connected:

  | MiniLab 3 | Model:Cycles |
  |---|---|
  | keys, pitch and mod strips | play the active track (auto channel) |
  | pads A1–A6 | the six pads, with velocity and pressure |
  | pads A7 / A8 | PLAY / STOP |
  | pads B1–B6 | select track 1–6 |
  | pads B7 / B8 | RECORD / TAP TEMPO |
  | knobs 1–8 | PITCH, DECAY, COLOR, SHAPE, SWEEP, CONTOUR, DELAY SEND, REVERB SEND |
  | faders 1–4 | LFO SPEED, VOLUME + DIST, SWING, CHANCE |

  Knobs and faders send absolute values; each step becomes one detent. Other MiniLab programs (or controllers)
  need their CC numbers in `MiniLab.swift`.

## Not done yet

See [RELEASE.md](RELEASE.md) for everything between this and a release.
