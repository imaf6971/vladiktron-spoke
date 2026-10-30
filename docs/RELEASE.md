# Road to a release

Last checked against the code on 2026-10-30. Latest release: [v0.1.0](https://github.com/imaf6971/vladiktron-spoke/releases/tag/v0.1.0).

## Where things stand

| Area | State |
|---|---|
| OS 1.13 on the virtual board | Boots and runs in real time |
| Screen, keys, knobs, pads, LEDs | Work |
| Audio out | Works, to the output chosen in Settings |
| MIDI over TRS and USB | Work as CoreMIDI ports; little tested with real gear |
| USB audio both ways | Works, routed through Mac audio devices |
| Arturia MiniLab 3 | DAW mode tested on hardware; Arturia mode only simulated |
| First run | Installs and checks the user's own `.syx` or `.zip` |
| Quit, Reset +Drive, Factory reset | Clean power-off through the OS's own path |
| Problems | Overlay with Restart for a fault, a crash or a hang; a bar for audio and microphone failures |
| CPU (M4 Pro) | Whole app about 14 % of a core idle; board alone about 13 % playing, 15 % with USB audio |
| Tests and CI | Firmware-free checks run on every push; the tests that boot the OS run on a developer's Mac |
| Licence | GPL-2.0-only, with third-party notices in the app (Help → Licences) |

## 1. Needed for a first release

### Distribution
Releases are zips of the app on the GitHub Releases page.

To make one:

1. Set the version in `app/Resources/Info.plist`, as `major.minor.patch`.
2. Add it to `CHANGELOG.md` and commit.
3. Tag it (`git tag v0.1.0`) and push the tag (`git push origin v0.1.0`).

The `release.yml` workflow then runs `dist.sh` and the firmware-free checks, and publishes the zip. To make the same
zip locally, run `./dist.sh`.

GPL-2.0 requires the source with each binary. Unicorn is vendored, so the source archive GitHub adds to each release
at its tag is the complete source.

### Must be tried at least once
- [ ] Run on a real macOS 14 Mac. It is built for 14 but has only run on macOS 27.
- [ ] Click through the first-run screen: the Open panel and drag and drop. So far it has only been seen in a snapshot
  and run by script (`SPOKE_INSTALL`).
- [ ] Click through the dialogs for Install OS…, Reset +Drive and Factory reset.
- [ ] Use USB audio input under the hardened runtime. macOS asks for microphone access again under the new bundle ID.
- [ ] Deny the microphone, and try an output that refuses to start, by hand.
- [ ] Hold Shift or Space and ⌘-Tab away, then type in the Open panel.
- [ ] Use a MiniLab 3 again since the board-handle change.
- [ ] Play for an hour on battery with `powermetrics`, and listen for the fans.

### Decided, keep it true
- **No App Store, no Developer ID.** The app stays ad hoc signed and is not notarised.
- **Elektron's firmware stays the user's.** The app never bundles or downloads it.
- **The names stay.** "Model:Cycles" and "Elektron" only say which device's OS the app runs, as Modded-Cycles does. The
  app's own name is Vladiktron Spoke. App Store and music software names were checked; trademark registers were not.
- **Apple Silicon only.** Intel would need a second Unicorn build, `lipo` and a test.
- **The panel's look** follows the hardware closely, and is the part most likely to draw a complaint. Change the
  artwork if Elektron ever asks.

## 2. Should be in 1.0

### Audio
- [ ] **Clock drift.** The board follows the Mac's clock, and the output device follows its own. Today a difference
  shows up as an occasional dropped or repeated buffer. Fix it with gentle resampling (±100 ppm) driven by how full the
  queue is, or through `CLOCK-PLAN.md`.
- [ ] **Input device drift.** An input clock that runs fast makes the app drop chunks, which clicks. It needs the same
  resampling.
- [ ] **A real audio device.** To appear as "Model:Cycles" in other apps, the app needs an AudioServerPlugIn: a HAL
  driver, installed separately and signed, that shares buffers with the app. Until then a DAW needs a loopback device
  such as BlackHole.
- [ ] **Level calibration.** MAIN VOLUME works, but its step size comes from the register layout (read as a DA7210's),
  not a measurement. Compare main volume, the headphone path and clipping with a real unit (`MCB_CODECLOG=1`).
- [ ] **Latency.** Measure pad click to sound and document it. Trim the 21 ms cushion if the clock work allows.
- [ ] **Buffer size.** The output device is chosen in Settings; the buffer size is still fixed.
- [ ] **Two engines on one device.** Routing *Model:Cycles to* the main output plays the mix twice, a few ms apart.
  Nothing breaks, but it sounds comb-filtered. Warn about it, or leave that device out of the menu.
- [ ] **Stream restarts at power-off.** The host restarts USB streams a few times while the OS powers off. It's
  harmless, but it could stop once the power key is held.

### CPU and battery
- [ ] **Fewer wake-ups.** About 6,300 context switches a second remain, and that is the main battery cost. Running ahead
  by one audio buffer is the likely fix (`CLOCK-PLAN.md`, `PERFORMANCE_FINDINGS.md`).
- [ ] **Less work per interrupt.** Each interrupt costs a full state restore and a slow vector fetch (F3). This is the
  largest remaining CPU item and the riskiest.
- [ ] **Late blocks under heavy load.** With 24 or 96 busy threads there were no underruns, but some audio blocks were
  up to 7 ms late, inside the 21 ms cushion. Joining the audio workgroup removed the lateness but raised CPU while
  playing from about 30 % to 50 % of a core. That work is parked on `wip/audio-workgroup`.

### MIDI
- [ ] **Real gear and DAWs.** Test Logic, Ableton and Bitwig: clock in and out, transport, program change, CC and NRPN,
  track channels, the auto channel, and MIDI out to a real synth.
- [ ] **SysEx backup.** Try Config → Backup both ways through both ports.
- [ ] **OS update over USB.** Decide whether it should work or be blocked with a message, since it would write a new
  OS to the app's flash image.
- [ ] **Clock out timing.** Bytes are sent "now". Stamp them with the time they left the UART or USB. Also test MIDI
  clock with the window hidden.
- [ ] **Empty SysEx keepalive.** The OS sends `F0 F7` over USB, as the real unit does. Check how DAWs react, and filter
  it if it causes trouble.
- [ ] **Unplug.** Add unplugging the USB cable at runtime, and test re-enumeration.
- [ ] **TRS lag after SysEx.** After a long SysEx, notes on TRS wait a few seconds. Find out if a real unit does too.

### Controllers
- [ ] **Other controllers.** A Settings screen to choose a controller and learn CCs, so the mapping isn't hard-coded.
- [ ] **MiniLab octave.** PANEL keys assume octave 0, and the MiniLab doesn't report Oct − / +.
- [ ] **Arturia mode** on a real MiniLab 3.

### App
- [ ] **Settings:** MIDI port names, controller mapping, USB connected or unplugged.
- [ ] **Menus:** a Reset (power cycle), and Help for the keyboard, mouse and MiniLab controls.
- [ ] **Window:** scaling, full screen, and how the panel looks at small sizes.
- [ ] **Accessibility:** VoiceOver labels on all controls, and knobs adjustable from the keyboard.
- [ ] **Status line:** a design pass, and perhaps audio and MIDI activity.
- [ ] **Faster quit.** About 2 s, because the OS wants the power key held for 13 polls. Setting its "power off now" flag
  (0x40a74560) would skip that, but means writing OS memory instead of pressing a key.

### Content
- [ ] **FACTORY presets are empty.** The OS rebuilds `/factory` on every mount from a block at eMMC block 0x180000
  ("MaGj", version 3) that only Elektron's factory writes. OS 1.12 and 1.13 don't carry it. Options: test Elektron
  Transfer so owners can copy in their own presets, or ship a freely licensed starter pack.
- [ ] Check the demo projects on a fresh +Drive against a real unit.

## 3. Hardware details still approximate
- **Pad velocity and pressure** come from one ADC level per pad. Compare with the real pads' curve.
- **Battery, auto-off and headphone detect** are not modelled. Battery sense reads "on mains".
- **Unmodelled peripherals** read back what was last written. Visit every menu page with faults logged to check that
  nothing new appears.
- **USB** models the high-speed device path only: no suspend and resume, stalls and halts, or full-speed hosts.
- **Timer 2.** The audio-block and SOF interrupts read scheduled event times from it (`dtim_count`). Keep that in mind
  for any other OS code that times itself against timer 2.

## 4. Tests and housekeeping
- [ ] `mcaudio_test`, `mcmidi_test` and `mcpanel_test` print results. Give them exact PASS/FAIL checks.
- [ ] Add a one-hour soak test: underruns, eMMC integrity, memory.
- [ ] Test project save, reload and delete through the menu, each surviving a reboot.
- [ ] Make `test.sh` rebuild the C tests it runs, so it can't run a stale one.
- [ ] Fix `mcusb_test`'s playback check, which fails under load.
- [ ] Give the MiniLab tests an injected clock instead of real sleeps.
- [ ] `mcmidi_test` and `mcaudio_test` assume the OS is ready 6 s after boot, so give them a fresh or cleanly shut down
  image.
- [ ] Make `emmc_save_ext` atomic and check its errors.
- [ ] Fix the thread race in `MiniLab.rescan`, and limit `MidiStreamParser`'s SysEx size.
- [ ] Add an option for `build.sh` not to install into the real data folder.
- [ ] Clear leftover debug files from `build/`, including the 1.9 GB `emmc_test.img`.

## Further reading
- [AUDIT.md](AUDIT.md): the code audit of 2026-10-28, with each fix and how it was checked
- [PERFORMANCE_FINDINGS.md](PERFORMANCE_FINDINGS.md): where the CPU goes, and what was measured
- [CLOCK-PLAN.md](CLOCK-PLAN.md): the plan for a board on its own clock, parked on `wip/board-clock`
