# What's left for a full release

State on 2026-10-26: the Model:Cycles OS 1.13 boots and runs in real time. The screen, keys, knobs, pads, LEDs and
audio out all work. So do MIDI over the TRS jacks and over USB (as CoreMIDI ports), USB audio both ways (routed to
Mac audio devices), and an Arturia MiniLab 3 can play it. Everything below is still open, ordered by how much it blocks a release.

## 1. Release blockers

### Legal and licensing
- **Unicorn is GPL-2.0, and it is linked statically.** Shipping the app means shipping it under GPL-2.0-compatible
  terms, with source (including `patch_unicorn.py`'s changes). The licence needs to be chosen and written down
  (`LICENSE`, about box, third-party notices).
- **The firmware decoding comes from Modded-Cycles (GPL-3.0).** Today it only runs at build time (`extract_os.py`).
  If it moves into the app (see onboarding below), it becomes GPL-3.0, or has to be rewritten from the format
  specification.
- **Elektron's firmware stays the user's.** The app must never bundle or download it. This is already true; keep it
  true in the onboarding flow.
- **Names and look.** "Model:Cycles", "Elektron" and a faithful copy of the panel are their trademarks and trade dress.
  This needs a decision (legal advice, or asking Elektron) before anything is published. The app name, window title
  and panel artwork may have to change.

### Onboarding without a developer toolchain
- Today `build.sh` needs git, Python, CMake, Ninja and Xcode, and it clones Unicorn and Modded-Cycles. A user needs
  instead: open the app → "Choose your `model-cycles_OS1.13.syx`" → the app decodes it (SysEx container, aPLib)
  and installs `main_os.bin` and `flash.bin` itself. That means porting the decoder to C or Swift (see licensing).
- Check the file is OS 1.13, by its version string and SHA-256 of the sections. Refuse anything else with a clear
  message: every address in `mcboard.c` is specific to 1.13.

### Clean shutdown
- Quitting stops the CPU wherever it is. If the OS was halfway through writing a project to the +Drive, the
  `emmc.img` can be left inconsistent. On quit, run the OS's own power-off path (the "Shutting down..." window) and
  wait for its last eMMC write. Then flush and close.
- Offer "Reset +Drive" and "Factory reset" in a menu, instead of "delete emmc.img and .extcsd" in the README.

### Signing and distribution
- Proper bundle ID, Developer ID signing, hardened runtime and notarisation. Unicorn's JIT needs
  `com.apple.security.cs.allow-jit` (and `MAP_JIT` on Apple Silicon). Verify it still runs once signed.
- Build with an Xcode project or a Swift package instead of `swiftc` in a shell script, with Unicorn and its patches
  pinned and vendored rather than cloned at build time.
- Decide on architectures: currently tested only on Apple Silicon (M4 Pro), macOS 14+. Intel needs a test, or an
  explicit "Apple Silicon only".

## 2. Should be in 1.0

### USB audio: make it a real audio device
Streaming works both ways: `mcusb_test` checks capture against the main mix and playback against a sweep sent in,
sample for sample. What remains:
- **A real audio device.** Today the streams go to or come from a device chosen in the USB Audio menu, which
  needs a loopback driver such as BlackHole for a DAW to use them. To appear as "Model:Cycles" in other apps the
  way the hardware does, the app needs an AudioServerPlugIn (a HAL driver, installed separately and signed) that
  shares its buffers with the app.
- **Starvation.** If the emulator thread is starved for more than 8 ms (heavy load on the Mac), the OS loses the
  streams' pace. The host then restarts them (SET_INTERFACE off and on), which leaves a short gap. Measure how
  often this happens on a busy laptop, and consider giving the CPU thread a real-time scheduling policy.
- **Fixed USB slice.** While USB audio streams, the CPU runs in 125 µs slices (one microframe) instead of 1 ms.
  Measure the extra CPU cost.
- **Input device drift.** The input device's clock can run faster than the Model:Cycles; the app then drops
  input chunks to keep latency down, which clicks. It needs resampling, as for the main output below.
- **Timing quirks.** Microframes and audio blocks are put in time order, and timer 2 reads in their two interrupts
  return the scheduled time of the event (see `dtim_count`). Keep this in mind for any other OS code that times
  itself against timer 2.

### Audio quality and timing
- **Clock drift.** Audio is paced by the host clock but played by the output device's clock. Today a drift shows up
  as an occasional dropped or repeated buffer. Replace that with gentle resampling (±100 ppm) driven by the fill
  level of the queue.
- **Level calibration.** Compare levels against a real unit: main volume, the headphone path, and the codec
  registers the OS writes over I2C (`MCB_CODECLOG=1`). Check full scale and clipping.
- **Audio device choice.** Pick the output device and buffer size in Settings, instead of the system default only.
- **Latency.** Measure end to end (pad click → sound) and document it. Trim the cushion if the clock work allows.

### CPU and power
- **The emulator keeps one core busy (~97 %) even when nothing plays**, measured on an M4 Pro. The idle loop at
  `0x400057e6` could be detected and the CPU thread put to sleep until the next timer or interrupt. That matters for
  laptops (battery, heat, fans).
- **App Nap.** Opt out with `ProcessInfo.beginActivity(.userInitiated, .latencyCritical)` while running, or audio
  and timers drift when the window is hidden.
- **Hiding the window.** Test audio and MIDI timing with the window minimised and in the background.

### MIDI
- **Sending timing.** Outgoing MIDI is collected every 1 ms and sent "now". Timestamp bytes with the time they left
  the UART or USB, for steadier clock out.
- **The empty SysEx keepalive.** The OS sends empty SysEx (`F0 F7`) packets over USB; the real unit does too. Check
  how DAWs react, and filter them if they cause trouble.
- **Cable unplug.** There is no "unplug the USB cable" at runtime. Add it (VBUS off → the OS's detach path) and
  test re-enumeration.
- **SysEx dumps and OS update.** Test project/pattern SysEx dumps (Config → Backup) through both ports in both
  directions. Decide whether an OS update over USB should work, or be blocked with a message, since the app would
  write a new OS to its own flash image.
- **Real-device testing.** Test with real MIDI gear and DAWs (Logic, Ableton, Bitwig): clock in and out, transport,
  program change, CC/NRPN, track channels, auto channel.

### MiniLab 3
- **Real-hardware testing.** The mapping was checked with a simulated MiniLab ("Minilab3 MIDI" source, factory
  values). It has not run against the real controller: confirm its port names, the pad channel, the pad pressure
  message type (poly vs channel), and the CC numbers.
- **The main encoder** (and its click) is not mapped. Find what it sends and map it to LEVEL/DATA.
- **Feedback to the controller.** The MiniLab's pad colours could follow the Model:Cycles' pad LEDs, and its screen
  could show the parameter name. Both are done over SysEx, using Arturia's protocol.
- **Endless knobs.** The MiniLab sends absolute values, so when its value is at 0 or 127 it can't move further.
  Either switch it to relative mode with SysEx or MIDI Control Center and decode that, or add pickup/soft takeover.
- **Other controllers.** A small Settings screen to choose the controller and learn CCs, so it isn't hard-coded.

### App and UI
- **Settings window:** audio device, MIDI port names, controller mapping, USB connected / unplugged.
- **Menus:** Reset, Factory reset, Reset +Drive, Reveal data folder, About (licences), Help (keyboard, mouse and
  MiniLab controls).
- **Window:** scaling and full-screen behaviour, a check on how the panel looks at small sizes, VoiceOver labels for
  the controls.
- **Status line.** The new bottom-edge status line (USB · controller) needs a design pass.

## 3. Hardware details still approximate
- **Pad velocity and pressure** come from a single ADC level per pad, harder toward its centre. Compare with the
  real pads' curve.
- **Power, battery and headphone detect.** The power button, battery level (Config → Battery), auto-off and
  headphone detect are not modelled. Check what the OS shows for them, and make the battery read "on mains".
- **Untouched peripherals.** Anything the OS touches that the board does not model only reads back what was last
  written. A full run of every menu page, with `on_unmapped` faults logged, should show nothing new.
- **USB edge cases.** USB is modelled for the high-speed device path only. Suspend/resume, stalls and halts, and
  full-speed hosts are not handled.

## 4. Tests and housekeeping
- **Automated checks.** `mcusb_test` already passes or fails. Turn `mcaudio_test`, `mcmidi_test` and the panel
  tests into checks like it (exact expectations, not printed numbers), run on every build, plus a one-hour soak test (audio underruns, eMMC
  integrity, memory).
- **Save and load.** Create, save, reload and delete projects; reboot and check they survive.
- **Factory content.** Check presets and demo projects on a fresh +Drive against a real unit.
- **Version control.** The project is not under version control yet: `git init`, commit, and write a
  CHANGELOG.
- **Clean `build/`.** Remove the leftover debug binaries and the 1.9 GB `emmc_test.img`. Keep test images in a temp
  folder.
