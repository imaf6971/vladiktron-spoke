# Audit — 2026-10-28

Scope: everything except the vendored Unicorn (`vendor/unicorn`, read only where this project's code depends on its
behaviour). About 6,300 lines: `mcboard.c` (1,874), the Swift app in `app/` (1,970), `mcfirmware.c`, the command-line
tools and tests.

## Progress

Quick wins (plan items 1–7) are done, one commit each, with a test that failed before the fix wherever
practical. `./build.sh` is clean. `./test.sh` passes: `mcunit_test` with 14 checks, `swift test` with 15 tests, and
`mctiming_test`. `mcshutdown_test` and `mcusb_test` pass too.

| Item | Commit | How it was checked |
|---|---|---|
| P1 status line redraw | `2af52fd` | builds, tests pass; **CPU before/after not measured yet** (your app was running) |
| R7 flash.bin size | `0c3dd2a` | new `mcunit_test`: the two size checks failed before the fix |
| R8 USB SysEx parser, T2 | `0709d36` | `mcunit_test` parser checks (one failed before), 6 new Swift parser tests |
| R9 unmapped hook, R10 USB play bound | `a0f2721` | `mcunit_test`: R9 failed and R10 overflowed the stack (SIGABRT) before; OS 1.13's dTDs measured at 7 frames, so streaming is unchanged |
| R11 allocation and thread checks | `7f7dddb` | build, `test.sh`, `mcshutdown_test` |
| R11 AudioOut leak | `cd2cea6` | build only |
| R2 quit during a reset | `d7b3bb3` | scripted run `SPOKE_TEST="12:reset,13:quit"`: before, still running at 45 s; after, quits 1 s after the request |
| R6 keys on deactivate and in dialogs | `d2af5f2` | build only: **click it through** (hold Shift, ⌘-Tab away; type in the Open panel) |
| U1 alert text | `73dbc2c` | build |
| D2 test hooks, `test.sh` total | `193bcb4` | scripted run: the route was not saved, and the script ran once across a reset |
| B1 Unicorn for macOS 14 | `50c36e3` | no linker warnings; Unicorn objects and the app binary say `minos 14.0`; `test.sh`, `mcusb_test`, `mcshutdown_test` pass. Not run on a real macOS 14 Mac. |
| R1 audio after an output change | `91ea0f6`, `6ab9722` | new `AudioOutTests`: stopping the engine and posting `AVAudioEngineConfigurationChange` left it stopped before the fix, and it restarts now, off the main thread (it ran on it at first and froze the window for seconds); a stopped output stays stopped. Tried by hand: the sound follows a headphone plug and unplug. Unplug → Speakers takes ~50 ms. Plug-in → External Headphones takes 1.6–2.1 s, all inside `engine.start()` while macOS readies the new jack device; the same start takes 3–6 ms once it has settled, so that wait is the hardware, not the app |
| R3 fault overlay with Restart | (these commits) | `mcunit_test`: `mcb_fault` reports an unmapped access after `mcb_run` returns; `BoardProblemTests` (4). Scripted runs with snapshots: `SPOKE_TEST="6:halt,14:restart,26:quit"` showed "The Model:Cycles stopped", Restart booted it again and the quit was a clean power-off; a 1-byte flash.bin showed "could not start" with the board's message. Not covered: an OS that hangs without an exception |

Left from R11: `emmc_save_ext` (not atomic), `MidiStreamParser`'s unbounded SysEx (belongs with R4), and the
`MiniLab.rescan` thread race. `README.md`'s Pieces table doesn't list `mcunit_test` yet: it had your
uncommitted edits, so I left it alone.

**New findings while implementing**
- **[done, see below] B1 — Medium, S. Unicorn is built for macOS 27, but the app says macOS 14+.** The linker warns about every
  Unicorn object: "built for newer 'macOS' version (27.0) than being linked (14.0)". `build.sh` doesn't pass
  `CMAKE_OSX_DEPLOYMENT_TARGET`, so Unicorn targets the build machine's OS. On an older macOS the app may fail to
  load or run. *Fix:* `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0` in `build.sh` (and remove `build/unicorn` once).
- **T6 — Low. `mcusb_test`'s playback check is flaky under load.** It failed 2 of 9 runs while your app was
  running; the committed code and the new code each passed 3 of 3 back to back. It's wall-clock bound; RELEASE.md notes
  the same after builds.

## 1. Architecture

**What it is.** A macOS app (Vladiktron Spoke) that runs Elektron's own Model:Cycles OS 1.13 on a virtual MCF54415
board. The firmware is never bundled: the user's `.syx`/`.zip` is decoded on first run.

**Pieces and data flow.**

```
 user's .syx/.zip ──► mcfirmware.c (SysEx → ELE3 container → aPLib → SHA-256 checks)
                        └► data folder: main_os.bin, flash.factory.bin, flash.bin
                                                     │
 SwiftUI app (app/)                                  ▼
  PanelView ── clicks/keys/scroll ──► Board.swift ──► mcboard.c (C, own thread: "Model:Cycles CPU")
  MiniLab.swift (CoreMIDI in) ──────►   (C calls)       ├ Unicorn m68k/ColdFire JIT runs the OS in 1 ms /
  MidiPort.swift (virtual ports) ───►                   │   125 µs slices; watchdog thread kicks slices
                                                        ├ MMIO models: INTC, PIT/DMA timers, GPIO, DSPI flash,
                                                        │   eSDHC+eMMC, eDMA, front-panel bus, UART9 MIDI,
                                                        │   USB device controller + a built-in USB host
                                                        ├ storage: flash.bin (whole-file atomic rewrite),
                                                        │   emmc.img (sparse, stdio) + .extcsd
                                                        └ lock-free SPSC rings out: audio, USB audio, MIDI
  AudioOut (AVAudioEngine) ◄── mcb_audio ───────────────┘
  UsbAudio (device routes) ◄─► mcb_usb_audio_read/write
  Board.refresh (60 Hz timer) ◄── mcb_screen / mcb_leds / mcb_status
```

Inputs reach the board through thread-safe setters (atomics for keys, encoders, pads; SPSC rings for MIDI). The
CPU thread is the only one that calls into Unicorn. Persistence is the OS's own: settings in SPI flash, projects and
presets on the eMMC (+Drive). Quit presses the OS's power key, so the OS saves before power-off.

**Build, run, test.** `./build.sh [syx]` builds Unicorn (CMake), the C tools (`cc`), the app (`swift build`), bundles
and signs it, and installs the firmware into the real data folder. `./test.sh` runs `swift test` (9 MiniLab tests)
and `./mctiming_test`. Other `mc*_test` tools are run by hand. No CI, linter or formatter is set up.

**Baseline run (this audit, M-series Mac, macOS 27):**

| Check | Result |
|---|---|
| `./build.sh` | ok, 5.6 s; no compiler warnings with the project's `-Wall -Wextra`; Swift build has no warnings |
| `./test.sh` | `swift test` 9/9 pass; `mctiming_test` all 9 checks ok (60 s) |
| `mcusb_test` (temp images) | 4/4 PASS |
| `mcshutdown_test` | all PASS |
| `mcpanel_test`, `mckeyscan` | not built by `build.sh`, but still compile |
| `clang --analyze` | 8 reports, all checked by hand: false positives (paths the code rules out) or dead stores |
| extra warnings (`-Wimplicit-fallthrough -Wformat-nonliteral`) | 2, both intended (`mcboard.c:462`, `mcfirmware.c:28`) |
| `mcrt_test`, `mcaudio_test`, `mcmidi_test` | not run (wall-clock bound or print-only) |

**What's done well (keep it):** `mcfirmware.c` checks bounds everywhere and gives a specific message for each way a
file can be wrong. flash.bin is saved atomically. The thread rules (only the CPU thread calls into Unicorn) are
written down and followed. The docs are unusually precise about OS addresses and why each model exists. Tests run
past the 32 s timer wrap. Mutation checks are part of the test culture.

## 2. Findings

Severity: **Critical** (data loss or crash in normal use) / **High** / **Medium** / **Low**. Effort: **S** (< 1 h),
**M** (half a day), **L** (days). Confidence says how sure I am that the problem is real.

I found nothing Critical. The worst items cost CPU on every frame, or show up when the Mac's audio route changes,
when you quit at the wrong moment, or when you send large SysEx.

### Performance

**[done 2af52fd] P1 — High, S. The whole panel redraws 60 times a second, even when nothing changes.**
`app/Board.swift:285` assigns `status = String(cString: st)` on every 60 Hz tick. `status` is `@Published`
(`Board.swift:9`), and its text always changes because it starts with the uptime (`t=12.3s`). `PanelView` observes
`Board` (`PanelView.swift:83`), so SwiftUI rebuilds the entire panel (around 80 views with gradients and shadows) every frame. No
view reads `status` (grep: only `Board.swift` uses it). *Why it matters:* the app takes about 33 % of a core against
22 % for the board alone (RELEASE.md), and some of that gap is likely this. *Confidence:* high that the panel
redraws; the CPU it costs is not measured yet. *Fix:* drop `@Published` from `status`, or update it only when asked
(e.g. once a second), and measure the CPU before and after.

**P2 — Low, M. Each flash command after a write rewrites all of flash.bin and fsyncs it, on the CPU thread.**
`mcboard.c:737` calls `flash_save` whenever chip select drops. `flash_save` (`mcboard.c:348`) writes the full 2 MB
and fsyncs. So saving one 4 KB sector (one erase plus 16 page programs) means about 17 × 2 MB writes and fsyncs while
the emulated CPU is stopped. *Evidence:* in an instrumented copy, `mcshutdown_test` (boot, change a project, power
off) wrote flash.bin **0** times, so this only happens when the OS saves settings. *Confidence:* high on the
mechanism, low on how much it matters. *Fix:* mark the image dirty and save it at most every ~0.5 s from the
between-slices loop, plus once at `mcb_run` exit and in destroy. That path already exists.

### Reliability and correctness

**[done, see Progress] R1 — High (if confirmed), S–M. Main audio probably does not recover when the Mac's output route changes.**
`AudioOut` (`app/AudioOut.swift:28-73`) starts an `AVAudioEngine` once and never watches
`AVAudioEngineConfigurationChange` (grep: nothing in `app/` handles it). AVAudioEngine stops itself when its output
hardware changes format, for example when headphones or an interface are plugged in or the default device changes
rate. The Model:Cycles would then go silent until the app is relaunched. *Confidence:* medium. This is how
AVAudioEngine is documented to behave, but I did not test it, because that means changing your system output.
*Fix:* first confirm it (play, switch the output in Control Center). Then, on the notification, rebuild and restart
the engine. The board's FIFO keeps running, so the existing cushion and resync logic covers the gap. The USB Audio
routes recover today through `UsbAudio.refresh()`, but only when the list of devices changes.

**[done d7b3bb3] R2 — Medium, S. Quitting during Reset +Drive / Factory reset (or during any shutdown) leaves the app stuck
mid-quit.** `Board.shutDown` returns without calling `done` if a shutdown is already running
(`Board.swift:126`). `applicationShouldTerminate` has already returned `.terminateLater` (`SpokeApp.swift:47-51`), so
`NSApp.reply(toApplicationShouldTerminate:)` is never sent. The reset's own closure then boots the board again. The
app is left in AppKit's "terminating" state with the quit never answered. *Window:* the ~2 s of a reset's shutdown.
*Confidence:* high (from reading the code). *Fix:* collect the `done` callbacks while a shutdown runs and call them
all, and let a pending quit stop the reset's reboot.

**[done, see Progress] R3 — Medium, M. An emulator fault is invisible, and there's no way to recover.** When Unicorn stops
(`mcboard.c:1664-1668`), `mcb_run` returns 1 and the CPU thread ends. The app keeps `board` non-nil and the screen
freezes. The fault text exists only inside the `status` string, which nothing shows (see P1). `Board.error` is shown
only when there is no screen image, in 6 pt type (`PanelView.swift:150-155`). The OS's own exception handler
(`mcb_os_exceptions`) isn't used by the app either. *Confidence:* high. *Fix:* poll `mcb_run`'s result (or the
thread finishing, or `mcb_os_exceptions > 0`) in `refresh()`, and show an overlay with the reason and a "Restart"
button that runs the existing `restart(changing:)` with no change.

**R4 — Medium, M. Large MIDI input is dropped silently (SysEx restore over either port).**
- TRS: `mcb_midi_in` (`mcboard.c:1753-1757`) drops any byte that doesn't fit its 4 KB ring. That ring drains at
  31,250 baud (3 KB/s), while a CoreMIDI client can deliver a whole dump in one callback.
- USB: `usb_event` (`mcboard.c:1427-1433`) drops packets past 4,096 events (~12 KB of SysEx). The host moves at
  most 128 packets per slice, and only when the OS has a buffer ready.
- Out: `MidiPortPair.emit` (`MidiPort.swift:46-54`) sends a whole SysEx as one `MIDIPacket`, whose length field is
  16-bit. `MIDIPacketListAdd` fails on anything over 64 KB, and the return value isn't checked.

*Why it matters:* Config → SysEx backup and restore is a normal Elektron workflow (RELEASE.md §2 MIDI lists it as
untested). *Confidence:* high that bytes are dropped; how big real dumps are is not checked. *Fix:* an unbounded
queue on the Swift side that feeds the C ring as it frees up (backpressure). Send long SysEx in chunks or through
`MIDIEventList`. A count of dropped bytes in `mcb_status`.

**R5 — Medium, S. Possible use-after-free of the board after a reset or quit, from MiniLab timers.** The MiniLab
closures call the C board directly through the raw pointer (`Board.swift:80-84`). Some timers keep the panel alive
strongly: pad B track select (`MiniLab.swift:242-244`, up to 110 ms) and strip taps (`MiniLab.swift:445`, 50 ms).
MIDI input stays live through the ~2 s shutdown, and `Board.stop()` → `mcb_destroy` frees the board. So a tap in the
last ~0.1 s before `stop()` writes into freed memory (`mcb_key` → `b->keys`). CoreMIDI destination callbacks
(`MidiPort.swift:33-38`) may have the same race against `MIDIEndpointDispose`; that part is unverified. *Window:*
narrow. *Confidence:* high for the MiniLab timers, low for CoreMIDI. *Fix:* send panel calls through `Board` methods
that check `board != nil` on one queue, or stop MIDI input at the start of `shutDown`.

**[done d2af5f2] R6 — Medium, S. Held keys can get stuck, and the keyboard shortcuts fire inside dialogs.**
`KeyboardMonitor` (`PanelView.swift:516-534`) tracks Shift (FUNC) and Space (PLAY) through local event monitors. If
the app loses focus while one is held, the key-up never arrives, so FUNC or PLAY stays down on the board. Latched
keys (`PanelView.swift:305-310`) also survive. Return is kept out of modal windows and sheets, but Space and Shift
are not. So typing in the Open panel's search field presses PLAY, and capital letters press FUNC.
*Confidence:* high. *Fix:* release FUNC, PLAY and DATA on `NSApplication.didResignActiveNotification`, and apply the
`modalWindow`/sheet guard to all three keys.

**[done 0c3dd2a] R7 — Low, S. An empty or odd-sized flash.bin crashes the board (division by zero).** `mcb_create`
(`mcboard.c:1474-1483`) takes whatever size the file has. `flash_xfer` then does `% s->size`
(`mcboard.c:376-387`), which is SIGFPE when the size is 0. `Board.start` only checks that the file exists
(`Board.swift:51`). *Confidence:* high. Unlikely to happen given the atomic writes, but the result would be a crash
on launch every time. *Fix:* check `size == MCF_FLASH_SIZE` in `mcb_create` and report "flash.bin is damaged —
Factory reset or Install OS".

**[done 0709d36] R8 — Low, S. The USB-MIDI SysEx parser emits a stale packet when SysEx is cut off on a 3-byte boundary.**
`mcb_usb_midi_in` (`mcboard.c:1773-1783`): after `sx_n` resets to 0, a status byte that ends the SysEx emits
`CIN 4+0 = 4` with leftover `sx[0..2]`. *Confidence:* high (from reading the code). *Fix:* emit only when `sx_n > 0`, plus a unit test.

**[done a0f2721] R9 — Low, S. The unmapped-memory hook has the wrong signature.** `on_unmapped` (`mcboard.c:912`) is `void`, but
`UC_HOOK_MEM_UNMAPPED` callbacks return `bool` (`unicorn.h:479`, used as `handled` in `cputlb.c:1611-1614`).
Whatever is left in the return register decides "handled". Today the CPU still stops, with `UC_ERR_MAP` instead of
`READ/WRITE_UNMAPPED`, and the fault text is already set, so this works by accident. *Fix:* return `false`.

**[done a0f2721] R10 — Low, S. The USB playback packet has no hard bound.** `usb_microframe` (`mcboard.c:1365-1372`) caps `frames`
at the OS's dTD size (up to 4,095 frames) but writes into `uint8_t buf[1024]` (128 frames). `play_acc` grows without
bound whenever the dTD is smaller than the feedback rate. An overflow needs a dTD over 1 KB **and** a large backlog,
which OS 1.13 doesn't produce. *Fix:* clamp to `sizeof buf / 8` and bound `play_acc`.

**R11 — Low, S. Smaller hygiene items.**
- `AudioOut.start` allocates a 64 KB `tmp` and never frees it (`AudioOut.swift:31`). One leaks per USB-audio route
  change or reset.
- `mcb_create`, `emmc_read` and `sd_command` don't check `calloc`/`malloc` (`mcboard.c:1460-1469, 474, 589, 606`).
  `pthread_create` isn't checked either (`mcboard.c:1650`).
- `emmc_save_ext` is neither atomic nor error-checked (`mcboard.c:404-407`). A torn file just falls back to the
  defaults, so this is harmless today.
- `MidiStreamParser` grows SysEx without limit if F7 never comes (`MidiPort.swift:130`).
- `MiniLab.rescan` changes `connected`/`destination` from the CoreMIDI notification thread and from `attach` on the
  main thread, partly outside `lock` (`MiniLab.swift:157-180`, `141-154`).
- Global state (`mmio_ctx`, the `hang_*` statics: `mcboard.c:659, 1604`) allows one board per process. That's fine
  for the app and tests, but it isn't written down in `mcboard.h`.

### Security

No material issues for a local, unsandboxed app. Notes:
- **Input parsing is careful.** `mcfirmware.c` bounds-checks every length, the depacker caps its output, and files
  are capped at 64 MB. `.zip` goes through `/usr/bin/ditto` into a fresh temp folder.
- **Any local process can drive the emulator through the virtual MIDI ports.** That's by design. It includes SysEx
  OS updates that would rewrite the emulated flash (already on RELEASE.md's list). Low.
- **The hardened runtime has only `allow-jit` and `audio-input`.** No unsigned-executable-memory exception. Good.
- **No secrets in the repository.** The firmware is git-ignored (`*.syx`, `*.img`).

### Testing gaps

**T1 — Medium, M. The firmware decoder has no tests, though it's the onboarding path every user takes.** Its
rejection messages (Model:Samples, other versions, damaged checksum, truncated file, non-Elektron) can be tested
with **synthetic** files built in the test (a SysEx header with product 0x0F, a bad checksum byte, a cut file). No
Elektron data would be needed, so these can run anywhere, CI included.

**[done 0709d36] T2 — Medium, S. The MIDI parsers have no tests.** `MidiStreamParser` (running status, real-time bytes inside a
message, SysEx) and `mcb_usb_midi_in` (byte → USB-MIDI event packets; R8 is in it) are pure functions.

**T3 — Medium, M. No CI.** Board tests need the user's firmware, so they can't run on GitHub. But `swift test`,
T1 and T2 can, along with a build check (Unicorn plus `swift build`). A broken build or mapping would then show up
on push.

**T4 — Low, S. The Swift tests sleep in real time** (`MiniLabTests.swift:54, 62, 71, 95`). The MiniLab code really
does depend on time (`systemUptime`, `asyncAfter`), so a loaded machine could make them flaky. Injecting a clock
would make them fast and exact.

**T5 — Low, M. Several tools print but don't check.** `mcaudio_test` and `mcmidi_test` print numbers and never fail
(RELEASE.md §4 knows). `mcpanel_test` and `mckeyscan` aren't built by `build.sh`. They still compile but nothing
keeps them working. `test.sh` doesn't rebuild the C tools, so it can run a stale `mctiming_test` if `build.sh`
wasn't run after an edit.

**Not covered at all:** Arturia-mode mapping, `Board` shutdown/restart sequencing (R2), audio-route changes (R1),
Factory reset file swap.

### UX and product gaps

- **[done 73dbc2c] U1 — Low, S. The reset alerts promise factory content that won't appear.** They say the OS "formats [the
  +Drive] with its factory content" (`SpokeApp.swift:108, 123`). Your uncommitted README edit now says the FACTORY
  folders stay empty.
- **[partly done: a board that won't start now shows an overlay] U2 — Medium, S. Several failures say nothing.** Microphone access denied (`UsbAudio.swift:126-131`) does
  nothing visible. A main output that fails to start only logs (`AudioOut.swift:70`; `Board.start` ignores the
  result). An emulator fault: see R3.
- **U3 — Low, M. Accessibility.** No accessibility labels on keys, knobs or pads, and knobs can't be adjusted from
  the keyboard (already on RELEASE.md's list).
- **U4 — Low, S. The status line shows only USB and controller.** No audio state, and no MIDI activity.

### Code health

- **C1 — Low, L. `mcboard.c` is one 1,874-line file holding about 12 peripheral models.** It reads well because of
  its comments, but it's hard to test in pieces. Splitting it (timers/INTC, storage, eDMA, USB, MIDI) would make
  unit tests like T2 easier.
- **C2 — Low, S. Duplicated code.** The PNG writer appears three times (`mcboot.c`, `mcpanel_test.c`,
  `mckeyscan.c`), the OS-file loader in every tool, and the MIDI packet sender twice (`MidiPort.swift:46`,
  `MiniLab.swift:493`).
- **C3 — Low, M. CoreMIDI's packet API.** It's `MIDIPacketList` (marked to-be-deprecated, no warning yet), and its
  16-bit packet length is part of R4.
- **C4 — Low, S. Intentional fall-through not annotated** (`mcboard.c:462`).

### Developer experience

- **D1 — Low, S. Every `./build.sh` with a `.syx` writes into your real data folder.** It never overwrites flash.bin
  or the +Drive, but a dry option such as `NO_INSTALL=1` would help for scratch builds.
- **[done 193bcb4] D2 — Low, S. Scripted runs overwrite your USB-audio choices.** `SPOKE_TEST` "out=/in=" steps go through
  `didSet`, which saves to UserDefaults (`Board.swift:107-108`, `UsbAudio.swift:56-58`). Test hooks shouldn't
  persist.
- **D3 — Low, S. `build/` holds leftover debug binaries and the 1.9 GB `emmc_test.img`.** RELEASE.md §4 already
  lists this.

## 3. Recommended action plan

Each step gets a failing test or measurement first where that's practical, and a separate commit.

**Quick wins (S, low risk)**
1. P1: stop publishing `status` every frame. Measure app CPU before and after with `top`/`ps`, back to back.
2. R7: check flash.bin's size in `mcb_create`, with a test.
3. R8: USB SysEx parser fix, with the T2 parser tests (C test for `mcb_usb_midi_in`, Swift test for `MidiStreamParser`).
4. R9, R10, R11 (leak, allocation checks, play clamp): one small commit each.
5. R2: queue the shutdown callbacks so a quit during a reset completes.
6. R6: release keys on deactivate, and keep Space/Shift out of dialogs.
7. U1: fix the alert text. D2: stop test hooks from persisting.

**High-impact fixes**
8. R1: confirm the audio-route problem by hand, then handle `AVAudioEngineConfigurationChange` (main and USB out).
9. R3 + U2: fault/exception overlay with Restart, and visible audio and microphone failures.
10. R5: route MiniLab panel calls through a board-alive check, and stop MIDI input when shutdown starts.
11. R4: MIDI input backpressure and SysEx-safe output, then the SysEx backup/restore test from RELEASE.md.
12. P2: debounced flash save.

**Tests and tooling**
13. T1: synthetic-file tests for `mcfirmware.c`. T3: GitHub Actions (build + `swift test` + C unit tests).
14. T4: injectable clock for MiniLab. T5: `test.sh` rebuilds what it runs; turn `mcaudio_test`/`mcmidi_test` into PASS/FAIL.

**Larger refactors (only if wanted)**
15. C1: split `mcboard.c` by peripheral, with no change in behaviour (`mcusb_test`/`mcshutdown_test`/`mctiming_test`
    as the safety net).
16. C3: move to `MIDIEventList` / `MIDIDestinationCreateWithProtocol`.

**Flags under the Phase 4 rules.** R7 changes `mcb_create`'s behaviour: it rejects a bad flash.bin. R4 changes what
happens to MIDI the board can't take in yet: it waits instead of being dropped. R3 adds a UI state. None of these
changes the C API's signatures or any file format.

## 4. Improvement and feature ideas

- **Back up and restore the +Drive (APFS clones).** `clonefile(2)` copies the 1.9 GB sparse `emmc.img` instantly
  and uses no extra space at first. A "Back up +Drive…" item, and an automatic snapshot before Reset/Factory reset,
  would make the destructive menu items undoable. Today they erase everything.
- **Play from the computer keyboard.** For example, two QWERTY rows for trigs 1–16, number keys for T1–T6, arrows
  for LEVEL/DATA. Then the app is playable on a laptop without a mouse or controller. The key layer already exists
  (`KeyboardMonitor`).
- **Record to WAV.** The main mix is already in a float FIFO. A "Record…" toggle that writes it to a file would
  capture ideas without BlackHole or a DAW.
- **Fault overlay with Restart (R3).** It turns a frozen window into a recoverable state, and it's the
  missing error state.
- **A Settings window** (already planned): output device and buffer size, MIDI port names, controller mapping, USB
  unplugged. Choosing the output device also covers the route problem in R1.
- **MIDI-learn for controllers.** This generalises the hard-coded MiniLab tables to other controllers (RELEASE.md
  lists it).
- **Longer term: an Audio Unit version.** It would load straight into a DAW, without loopback devices or the HAL
  driver RELEASE.md mentions. It's the biggest change, but it would make USB audio routing unnecessary.
