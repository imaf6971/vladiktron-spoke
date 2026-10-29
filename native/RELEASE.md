# What's left for a full release

State on 2026-10-27: the Model:Cycles OS 1.13 boots and runs in real time. The screen, keys, knobs, pads, LEDs and
audio out all work. So do MIDI over the TRS jacks and over USB (as CoreMIDI ports), USB audio both ways (routed to
Mac audio devices), and an Arturia MiniLab 3 can play it. Quitting is a clean power-off through the OS's own power-key
path (`mcshutdown_test` checks that the project survives it), and the app menu has Reset +Drive, Factory reset and
Reveal Data Folder. On first run the app installs the OS from the user's own `.syx` or Elektron's `.zip`
(`mcfirmware.c`), checking it is the original 1.13. The CPU thread sleeps while the OS idles, so the app takes about a third of one core
instead of all of it. Everything below is still open, ordered by how much it blocks a release.

## 1. Release blockers

### Legal and licensing
Done (2026-10-27): the project is GPL-2.0-only (`LICENSE` at the repository root, the same terms as Unicorn, which
is linked statically). `THIRD-PARTY-NOTICES.md` lists Unicorn (GPL-2.0, GLib parts LGPL-2.0) and the MIT code this
builds on, with the MIT texts: Modded-Cycles (`mcengine.c`, the EMAC model, OS addresses, and `mcfirmware.c` by way
of `mtlib`), drumkilla's elektron-model-tweaks and Marcel Bierling's elektron-firmware-tool. Modded-Cycles is MIT,
not GPL-3.0 as this file used to say (checked at the pinned revision 2594419; its one GPL-3.0 script is not used
here). The app bundles both files (Help → Licences) and its About panel names the licence, the source and the
trademark disclaimer.

Decided: "Model:Cycles" and "Elektron" stay, used only to say which device's OS the app runs, as Modded-Cycles
does; the app's own name is Vladiktron Spoke (renamed from Model Mac on 2026-10-27, after checking the App Store
and music software for clashes; trademark registers not yet searched), the panel carries its own wordmark and the
label "Spoke", and the About panel and notices say it
is not affiliated with Elektron. Still open:
- **Source with each binary.** GPL-2.0 needs the complete source with a binary release, Unicorn included. Since
  Unicorn is vendored (with its changes applied and listed in `vendor/`), the repository at the release tag is that
  source: attach `git archive --format=zip -o vladiktron-spoke-<version>-source.zip <tag>` to every release.
- **Elektron's firmware stays the user's.** The app must never bundle or download it. This is true today; keep it
  true.
- **The panel's look** follows the hardware closely (layout, colours, labels). That is the part of the trade dress
  most likely to draw a complaint; change the artwork if Elektron ever asks.

### Onboarding
Done: with no OS in its data folder the app shows a first-run screen (choose or drop the `.syx`, or Elektron's
`.zip`, with a link to Elektron's support page). `mcfirmware.c` decodes it in about 10 ms and checks the version
string and the SHA-256 of the container and of the decoded MAIN OS; it rejects other versions, modified 1.13s,
the Model:Samples OS, damaged or truncated files and non-Elektron files, each with its own message. The output is
byte-identical to the old Python extraction. `build.sh` uses the same code (`mcinstall`), so it no longer needs
Python or the Modded-Cycles clone for the firmware. What remains:
- **A sandboxed build** (if the app goes to the Mac App Store) would need the Open panel's security-scoped URL
  and no `ditto` subprocess for the `.zip`.
- **First-run copy** names Elektron and links its site; it is part of the names-and-look decision above.
- The first-run screen has been checked in a snapshot and the install by a scripted run (`SPOKE_INSTALL`); the
  Open panel and drag and drop have not been clicked through.

### Signing and distribution
Done (2026-10-27): Unicorn is vendored in `vendor/unicorn` (2.1.4 with this project's changes applied; `vendor/README.md`
says what changed), so the build downloads nothing and needs only CMake and the Xcode command line tools: no git,
Python or Ninja. The app is a Swift package (`Package.swift`: the C board as module `MCBoard`, the SwiftUI app as an
executable target); `build.sh` builds Unicorn with CMake, runs `swift build`, wraps the bundle and signs it with the
hardened runtime and `app/Spoke.entitlements` (`com.apple.security.cs.allow-jit`, and
`com.apple.security.device.audio-input`, which the hardened runtime needs for the USB Audio input). Checked with an
ad hoc signature: the OS boots and plays with no underruns, and quits cleanly. Signed without `allow-jit`, Unicorn
stops at start-up ("Could not allocate dynamic translator buffer"), so the entitlement is what makes it work. CPU
while playing is unchanged: 38–44 % of a core for the new build with or without the hardened runtime, 39–42 % for
the old `swiftc` build (M4 Pro, measured back to back; runs right after a build read up to 59 %, for any build).
What remains:
- **Developer ID and notarisation** (needs a paid Apple Developer account): `SIGN_ID="Developer ID Application: …"
  ./build.sh` signs with it (with the secure timestamp notarisation requires), then `ditto -c -k --keepParent "build/Vladiktron Spoke.app" Spoke.zip`, `xcrun notarytool submit
  Spoke.zip --keychain-profile <profile> --wait` and `xcrun stapler staple "build/Vladiktron Spoke.app"`.
- **The USB Audio input under the hardened runtime** has its entitlement, but has not been tried with a real input
  since (macOS asks for microphone access again, under the new bundle ID).
- **Architectures:** tested only on Apple Silicon (M4 Pro), macOS 14+. The build is arm64 only; Intel would need a
  second Unicorn build and `lipo`, and a test. Otherwise say "Apple Silicon only".
- **A DMG or zip** for downloads, and a version number scheme (`Info.plist` says 0.1 (1)).

## 2. Should be in 1.0

### USB audio: make it a real audio device
Streaming works both ways: `mcusb_test` checks capture against the main mix and playback against a sweep sent in,
sample for sample. What remains:
- **A real audio device.** Today the streams go to or come from a device chosen in the USB Audio menu, which
  needs a loopback driver such as BlackHole for a DAW to use them. To appear as "Model:Cycles" in other apps the
  way the hardware does, the app needs an AudioServerPlugIn (a HAL driver, installed separately and signed) that
  shares its buffers with the app.
- **Starvation.** If the emulator thread is starved for more than 8 ms (heavy load on the Mac), the OS loses the
  streams' pace. The host then restarts them (SET_INTERFACE off and on), which leaves a short gap. Measured
  2026-10-29 (M4 Pro, the app playing for a minute, 24 or 96 threads spinning at user-interactive QoS): no
  underruns and no stream restarts, with or without USB audio; 116–165 audio blocks per run were more than 2 ms
  late, the worst 4.8–7.0 ms, all absorbed by the 21 ms cushion. Joining the main output's audio workgroup (with a
  time-constraint policy, as gearmulator does) took that to 0 late blocks, worst 0.2 ms, but raised the app's CPU
  while playing from ~30 % to ~50 % of a core. Joining alone does it (not the policy), and the extra time is on the
  main thread, not the board's CPU thread, which does the same work either way; the cause is not found. That
  work is on the local branch `wip/audio-workgroup`. A board on its own emulated clock, run ahead one IO period
  per audio callback, would wake the CPU thread tens of times a second instead of thousands, which suits a
  real-time thread better; revisit this after that.
- **Fixed USB slice.** While USB audio streams, the CPU runs in 125 µs slices (one microframe) instead of 1 ms:
  about 6200 slices/s instead of 800, and the microframes take about 3–8 ms of host time per second (M4 Pro,
  `MCB_RTLOG=1`). With nothing routed, microframes take ~0.15 ms/s. Since the idle sleep, streaming costs
  about the same CPU as not streaming (`mcrt_test`: 26 % against 23 %).
- **Two engines on one device.** Routing *Model:Cycles to* the same device as the main output (MacBook Pro
  Speakers, say) plays the mix twice, a few ms apart: it sounds louder and comb-filtered. Nothing breaks (both
  queues keep their cushion), but the menu could warn about it, or leave out the main output's device.
- **Input device drift.** The input device's clock can run faster than the Model:Cycles; the app then drops
  input chunks to keep latency down, which clicks. It needs resampling, as for the main output below.
- **Timing quirks.** Microframes and audio blocks are put in time order, and timer 2 reads in their two interrupts
  return the scheduled time of the event (see `dtim_count`). Keep this in mind for any other OS code that times
  itself against timer 2. Only those two routines read it today (0x400589ce, 0x40004326); 0x4005131a and
  0x40053376 also read it, but were not seen running.
- **Restarts at power-off.** While the OS powers off it stops taking the audio interrupt, so the microframes
  stall and the host restarts the streams a few times (`MCB_USBLOG=1`). Harmless, but the host could stop
  restarting once the power key is held.

### Audio quality and timing
- **Clock drift.** Audio is paced by the host clock but played by the output device's clock. Today a drift shows up
  as an occasional dropped or repeated buffer. Replace that with gentle resampling (±100 ppm) driven by the fill
  level of the queue.
- **Level calibration.** Compare levels against a real unit: main volume, the headphone path, and the codec
  registers the OS writes over I2C (`MCB_CODECLOG=1`). Check full scale and clipping.
- **Audio device choice.** Pick the output device and buffer size in Settings, instead of the system default only.
- **Latency.** Measure end to end (pad click → sound) and document it. Trim the cushion if the clock work allows.

### CPU and power
Done: the CPU thread sleeps while the OS idles in its background loop (`IDLE_PC`, 125–250 µs at a time). Two
Unicorn patches cut what the OS costs to run: RAM stores without code on their page take the fast path again, and
the JIT stays executable for a whole run. The app opts out of App Nap (`beginActivity`, idle system sleep still
allowed). On an M4 Pro: the board alone goes from 100 % to about 22 % of a core idle and 23 % playing
(`mcrt_test`, no block more than 1.6 ms late). The whole app goes from ~97 % to about 33 % idle, the same with the
window hidden, with no underruns. What remains:
- **Battery.** Measure power on battery (`powermetrics`) and fan noise in a long session; the 12 kHz panel scan
  and 1500 audio blocks a second still wake the CPU thread several thousand times a second.
- **Less work per interrupt.** Each interrupt costs a TB-state restore (`cpu_restore_state`, ~7 % of the CPU
  thread's time) and the vector fetch goes through the slow load path. Unicorn changes, if more is needed.
- **Hidden-window MIDI timing.** Audio was checked with the app launched hidden (no underruns); MIDI clock
  timing with the window hidden still needs a test with real gear.

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
- **DAW mode** (app/MiniLab.swift) was tested on a real MiniLab 3: display, pad colours and LED feedback, endless
  knobs, main encoder, both pad banks, the PANEL keys and Shift as FUNC. The pads send no pressure in DAW mode (none
  seen). Arturia mode was only checked with a simulated source.
- **Octave.** PANEL keys assume octave 0 (lowest key note 48); the MiniLab does not report Oct − / +.
- **Other controllers.** A small Settings screen to choose the controller and learn CCs, so it isn't hard-coded.

### App and UI
- **Settings window:** audio device, MIDI port names, controller mapping, USB connected / unplugged.
- **Menus:** Reset (a power cycle), About (licences), Help (keyboard, mouse and MiniLab controls). Install OS…,
  Reset +Drive, Factory reset and Reveal Data Folder are done; their dialogs have not been clicked through in a scripted test yet.
- **Quitting takes about 2 s** (the OS needs the power key held for 13 polls before it shuts down). It could be shorter
  by setting the OS's own "power off now" flag (0x40a74560, set at 0x4005398c and checked at 0x400538f4), which
  skips the wait. That would mean writing OS memory instead of pressing a key.
- **Window:** scaling and full-screen behaviour, a check on how the panel looks at small sizes, VoiceOver labels for
  the controls.
- **Status line.** The new bottom-edge status line (USB · controller) needs a design pass.

## 3. Hardware details still approximate
- **Pad velocity and pressure** come from a single ADC level per pad, harder toward its centre. Compare with the
  real pads' curve.
- **Power, battery and headphone detect.** The power key is modelled (port A bit 2, polled at 0x400538a2), and
  the battery sense (port A bit 3) reads "on mains". Battery level (Config → Battery, ADC at 0x422ff164),
  auto-off and headphone detect are not modelled. On battery with a low reading, the OS takes its low-battery
  shutdown (state 10 from 0x400530ca, the ShutdownNotificationWindow at 0x4003ea34).
- **Untouched peripherals.** Anything the OS touches that the board does not model only reads back what was last
  written. A full run of every menu page, with `on_unmapped` faults logged, should show nothing new.
- **USB edge cases.** USB is modelled for the high-speed device path only. Suspend/resume, stalls and halts, and
  full-speed hosts are not handled.

## 4. Tests and housekeeping
- **Automated checks.** `mcusb_test`, `mcrt_test`, `mcshutdown_test` and `mctiming_test` already pass or fail. Turn `mcaudio_test`,
  `mcmidi_test` and the panel tests into checks like it (exact expectations, not printed numbers), run on every build, plus a one-hour soak
  test (audio underruns, eMMC integrity, memory).
- **CI.** Done (2026-10-29): GitHub Actions builds everything and runs `mcunit_test`, `mcfirmware_test` (the decoder's messages, with
  made-up files) and `swift test` on every push and pull request. The board tests need the user's firmware, so they still run on a
  developer's Mac only.
- **Save and load.** `mcshutdown_test` checks that the working project survives a clean power-off. Still open:
  saving, reloading and deleting projects through the project menu, and checking that each survives a reboot.
- **Test images.** `mcmidi_test` and `mcaudio_test` assume the OS is ready 6 s after boot. On an image left by a hard
  stop, the next boot takes longer, so give them a fresh or cleanly shut down image.
- **Factory content.** The FACTORY preset folders are empty. On every mount the OS (0x400a7964) recreates
  `/factory/<BD..TO>` and writes 512 `.mc-snd` files from a block at eMMC block 0x180000 ("MaGj", version 3, type
  0x15, CRC-checked), which only Elektron's factory writes; neither OS 1.12 nor 1.13 carries it. Options: test
  Elektron Transfer against the USB-MIDI port, so owners can copy their own presets in (not into `/factory`, which
  the OS rebuilds), or ship a starter pack of freely licensed presets. Also check the demo projects on a fresh
  +Drive against a real unit.
- **CHANGELOG.** The project is in git now (repository root); a CHANGELOG is still to be written.
- **Clean `build/`.** Remove the leftover debug binaries and the 1.9 GB `emmc_test.img`. Keep test images in a temp
  folder.
