# Plan: the board on its own clock

Written 2026-10-29. No code has changed for this yet. Each phase below is a separate, mergeable step that keeps
the app working, with a check that has to pass before the next one starts.

## Why

Everything inside the board is timed by the Mac's clock. `now_us() - b->t0` is read in 11 places in `mcboard.c`,
and the time it gives drives the PITs and DMA timers, the counter the OS reads (`dtim_count`), the audio block
schedule (`dma50_due`), MIDI bytes at 320 µs, the USB microframe schedule and its general-purpose timers, the
render hook, the flash save debounce, the screen snapshot and the status line; the busy-time counter times slices
with `now_us()` too. CPU slices end when a watchdog thread
kicks Unicorn every 1 ms (125 µs while USB audio streams), and the CPU thread sleeps in `idle_sleep` until the
next event in real time.

So emulated time and real time are the same thing, and that costs us:

- **Slow tests.** Nothing headless can run faster than real time: `mctiming_test` takes about 70 s,
  `mcshutdown_test` about 16 s.
- **Flaky tests.** A test's outcome depends on how busy the Mac is. `mcusb_test`'s playback check failed 2 of 9
  runs while the app was running (T6), and checks run right after a build are known to fail.
- **No determinism.** The same inputs do not give the same run twice, so a bug seen once can't be replayed.
- **Hardware model and real-time policy are mixed.** Lateness counters, resyncs after 20 ms, USB stream
  restarts after an 8 ms stall, the special cases in `dtim_count` for the two interrupt routines that stamp
  timer 2, the ordering rules between microframes and audio blocks: all exist because the hardware model runs
  late against the clock it is modelling.
- **It blocks the next steps.** Clock drift (RELEASE.md), MIDI timestamps, an Audio Unit version and a cheaper
  real-time thread (the parked `wip/audio-workgroup`) all want a board that can be told "run until time T".

## The idea

The board keeps its own time, **E** (emulated microseconds since boot). E moves forward only by what the board
does:

- **While the CPU runs,** E advances by the instructions executed divided by a fixed speed, **K** instructions
  per µs. Same code path, same instructions, same E.
- **When the OS is idle** (the CPU reaches `IDLE_PC`, the `bra .` its background loop spins in), nothing can
  happen before the next timer, DMA block, MIDI byte or microframe, so E jumps straight to that event
  (`next_event` already computes it).

Every hardware model reads E, never the Mac's clock. A separate piece, the **pacer**, keeps E in step with real
time for the app. Headless tests don't use a pacer: they say "run until E = 40 s" and that takes as long as the
work takes, not 40 s.

```
             today                                    after
  ┌──────────────────────────────┐      ┌───────────────────────────┐   ┌─────────────────────┐
  │ board                        │      │ board                     │   │ pacer (app only)    │
  │  timers ── now_us()          │      │  timers ── E              │   │  E ≈ host time      │
  │  audio  ── now_us(), resync  │  →   │  audio  ── E              │ ← │  sleep, catch up,   │
  │  USB    ── now_us(), restart │      │  USB    ── E              │   │  drift, lateness    │
  │  slices ── watchdog kicks    │      │  slices ── budget of insns│   └─────────────────────┘
  │  idle   ── nanosleep         │      │  idle   ── E jumps        │   tests: no pacer, as fast
  └──────────────────────────────┘      └───────────────────────────┘   as the host can go
```

## Design

### Counting instructions

Unicorn has no cheap counter. `uc_emu_start(..., count)` installs a code hook on **every instruction**, far too
slow. Two ways, both measured in phase 0 before choosing:

Unlike QEMU, Unicorn's `gen_tb_start` does not test `icount_decr` inline: every block calls a helper,
`check_exit_request` (`qemu/accel/tcg/tcg-runtime.c`), because an inline branch there breaks TCG's liveness
analysis (Unicorn's comment in `gen-icount.h`). So QEMU's `CF_USE_ICOUNT` subtraction cannot simply be turned on.
Two ways, both measured in phase 0 before choosing:

1. **Block hook (almost no Unicorn change).** A `UC_HOOK_BLOCK` callback adds each executed block's instruction
   count to a counter and calls `uc_emu_stop` once the slice's budget is spent. Stock Unicorn passes the block's
   size in bytes (`translator.c` patches the `0xf8f8f8f8` placeholder with it); patching in `num_insns` instead
   is one line and gives exact counts. Cost: a second helper call per block, on top of `check_exit_request`.
2. **Count in the exit helper (a Unicorn patch).** Give `check_exit_request` a second argument, the block's
   instruction count, patched in at `gen_tb_end` the way QEMU patches `icount_start_insn`. The helper subtracts
   it from the slice's budget and exits when the budget runs out. Exact counts, no extra call per block. It
   extends the patch we already maintain in `vendor/` (`patch_unicorn.py` already changes this helper).

Either way the count is charged when a block starts. A block left part-way (an exception, `cpu_loop_exit_restore`)
is still charged in full; that is deterministic, but phase 0 checks how often it happens and whether the restore
path should give back the instructions not run.

Either way the budget, not the watchdog thread, ends a slice, so the watchdog goes away. `mcb_stop` from another
thread keeps using `uc_m68k_kick`.

### Slices end at the next event

Events (timers, DMA blocks, MIDI bytes, microframes) fire between slices, so a slice must end exactly when the
next one is due: its budget is `min(1 ms, next_event − E) × K` instructions. With fixed 1 ms slices events would
still land up to a slice late in E, and the `dtim_count` special cases would still be needed. The cost is more
`uc_emu_start` calls: 8000 a second from microframes alone while USB audio streams, plus timers. Phase 0
measures what starting and ending a slice costs.

### The CPU speed, K

K is part of the model now. It decides how much emulated time the OS's own work takes: rendering a 32-frame
audio block, servicing the 12 kHz panel scan, formatting the +Drive. Today the JIT runs the OS as fast as the Mac
allows, so that work takes almost no emulated time at all, which is not what the hardware does either.

The MCF54418 runs at 250 MHz. Phase 0 measures how many instructions the OS executes per audio block and per
second while playing, then picks K so that:

- the OS's work fits its own deadlines with the margin the real unit has (a block is 667 µs), and
- the Mac can run K × (the OS's busy fraction) instructions per second in real time with room to spare.

If those conflict, K favours keeping up in real time. K is one constant, easy to change and to test with.

The real unit's margin can't be measured here, and the OS's instruction count says nothing about its cycles per
instruction, so K is an estimate. Phase 2 therefore checks that the board behaves the same at K / 2 and 2 K;
if it doesn't, behaviour depends tightly on K, which is a finding in itself.

### Timer reads inside a slice

`dtim_count` returns E at the instruction that reads it: E at the start of the slice plus the instructions run so
far divided by K. The two special cases for the audio-block and SOF interrupt routines (they return the time the
event was due) should become unnecessary, because slices end at the next event, so events happen exactly when
they are due in E. That has to be
checked against the OS's USB stream alignment (`mcusb_test`) before they are removed.

### The pacer

In the app, one thread runs the board in a loop:

1. Target = E of the last sync + (host time since then) × rate.
2. Run the board until E reaches the target (each slice ends at the next event, at most 1 ms of E).
3. Sleep until the host clock reaches the next target.

What moves out of the board into the pacer:

- **Lateness.** If the board falls behind the host, today's per-block resync (`dma50_due`, after 20 ms) and USB
  restart (after 8 ms) handle it inside the model. Instead the pacer decides: catch up by running without sleeping
  until E is back in step, or, past a limit, accept the gap. The audio FIFO tells it how far behind it is.
- **Clock drift.** The output device consumes at its own clock. The pacer nudges `rate` (±100 ppm; this is
  only for drift, not for catching up) to hold the
  FIFO near the cushion. The emulated Model:Cycles then runs a hair fast or slow, inaudibly, instead of the app
  dropping or repeating buffers. This **replaces** the clock-drift item in RELEASE.md and needs no resampler.
- **Run-ahead.** At first the pacer keeps today's behaviour: E stays within ~1 ms of the host. Later it can run
  ahead one output IO period per wake-up, which means tens of wake-ups a second instead of thousands. That makes
  the parked audio-workgroup change worth trying again.

### Inputs

Input calls (keys, knobs, pads, MIDI, USB-MIDI, USB audio in) keep their API and land at the board's current E,
as they do now between slices. In the app they come from other threads, so the E they land at depends on
scheduling; a headless test that wants a repeatable run injects them from its own thread between
`mcb_run_until` calls. Later (phase 6) they can carry a host timestamp the pacer converts to E, which is
the MIDI-timestamp item.

### The C API

| Call | Meaning |
|---|---|
| `mcb_run_until(b, e_us)` | new: run until E ≥ `e_us` (no sleeping, no host clock). Headless tests use this. |
| `mcb_time(b)` | new: E in µs. |
| `mcb_run(b, seconds)` | unchanged meaning: real time, now a thin pacer around `mcb_run_until` (0 = until `mcb_stop`). The app and `mcrt_test` keep calling it. |

## Phases

Every phase ends with `build.sh` and `test.sh` passing, `mcrt_test` passing on an idle Mac, `mcusb_test` and
`mcshutdown_test` passing, and a one-minute scripted app run with no underruns. "App CPU" is measured as in the
workgroup trial: CPU while playing, mean of 10 samples, compared back to back with `main`.

### 0. Measure (no behaviour change; throwaway code)

- How many instructions the OS runs per audio block and per second, idle and playing (a block hook in a scratch
  tool).
- What a block hook costs: `mcrt_test`'s board CPU with and without one.
- Whether counting in `check_exit_request` works and what it costs.
- What starting and ending a slice costs, and how many slices a second ending at the next event takes, idle,
  playing and with USB audio streaming.
- How often a block is left part-way (exceptions), so charged for instructions it did not run.
- **Decide:** counting method, and K.

### 1. One time source (pure refactor)

Replace every `now_us() - b->t0` in the hardware models with `board_time(b)`, which still returns the Mac's time.
No behaviour change, but every reader is now in one place. Check: all tests pass as before.

### 2. Emulated time, still paced by the host

- Slices end on the instruction budget, set so a slice ends at the next event; the watchdog thread is removed.
- E advances by instructions / K while the CPU runs and jumps to the next event at `IDLE_PC`.
- `mcb_run` becomes the first pacer: it keeps E within 1 ms of the host clock and sleeps when it is ahead,
  as `idle_sleep` does now.
- `dtim_count` reads E with the in-slice instruction count.

This is the risky phase. **Must hold:** `mcrt_test` passes; app CPU within 10 % of `main`; no underruns, and the
same late-block numbers or better under the 24- and 96-thread loads from the workgroup trial; `mcusb_test`
bit-exact in both directions; tap tempo and knob checks unchanged; the same checks pass at K / 2 and 2 K.

### 3. Headless tests run on E

- Add `mcb_run_until` and `mcb_time`.
- Convert `mctiming_test`, `mcshutdown_test`, `mcusb_test`, `mcmidi_test` and `mcaudio_test` to run on E. The
  clock-during-SysEx check paces its clock bytes by E instead of the wall clock.
- New `mcdeterminism_test`: the same script twice (boot, play, knob turns, MIDI, a SysEx), every input injected
  from the test's thread at a set E; the audio and DDR hashes must match.
- **Must hold:** the same checks pass; `mctiming_test` drops from ~70 s to a few seconds (target: under 10 s);
  T6 is gone (`mcusb_test` 20 of 20 under load).

### 4. Real-time policy leaves the board

- Remove from the hardware models: the per-block resync and lateness counters, the 8 ms USB stall restart, and
  the `dtim_count` special cases (only if `mcusb_test` still aligns the streams without them). Each removal is
  its own commit, checked by `mcusb_test`.
- The pacer owns lateness: it counts how far E is behind the host and decides when to catch up. `MCB_RTLOG`
  reports the pacer's numbers.
- **Must hold:** as phase 2.

### 5. Drift following

- The pacer adjusts its rate from the main output FIFO's fill level.
- **Must hold:** an hour-long run on an external device with a different clock (or the USB Audio output, which
  runs off the board's clock) shows no drops or repeats in `SPOKE_AUDIOLOG` and a stable queue; the clock-drift
  item in RELEASE.md is done.

### 6. Later, built on this (separate plans)

- Run ahead one IO period per wake-up, then retry `wip/audio-workgroup`.
- MIDI input at its CoreMIDI timestamp, MIDI output stamped with E converted to host time.
- An Audio Unit: the host's render callback becomes the pacer ("run until E covers this block").

## Risks

- **Real-time audio regresses.** Phase 2 changes how the CPU thread is scheduled. Mitigation: the phase-2 checks
  above, measured under load, with `main` side by side; the old wall-clock path stays selectable by an
  environment variable until phase 4 lands, so it can be compared and rolled back.
- **Counting costs too much CPU.** Phase 0 measures it before anything is built; counting in the exit helper
  exists as the cheaper route.
- **Slices that end at every event cost too much.** Thousands of `uc_emu_start` calls a second while USB audio
  streams. Phase 0 measures it; if it is too high, fast timers keep being fired from register accesses inside a
  slice, as `pit_check` already does.
- **The wrong K changes how the OS behaves.** Too low and the OS's work misses its own deadlines (audio the
  hardware never drops); too high and the Mac can't keep up. Phase 0 picks it from measurements; the phase-2
  checks (tap tempo, knob steps, audio, USB) catch the effects.
- **USB stream alignment.** The OS aligns USB audio to timer-2 stamps of two interrupts; the current workarounds
  exist because events happened late. Removing them is phase 4 and is checked one at a time by `mcusb_test`'s
  bit-exact streams.
- **A larger Unicorn patch to maintain** (if counting in the exit helper is chosen). It is a few lines in code we already
  patch, documented in `vendor/README.md`.

## Open questions

- K: from phase 0.
- Block hook or counting in the exit helper: from phase 0.
- How far may the pacer let E fall behind before it stops catching up and accepts a gap? Start at today's 20 ms.
- Should the screen snapshot (every 16 ms) and the flash save debounce (0.5 s) use E or host time? E is simpler
  and deterministic; the UI only notices if E runs far from real time, which the pacer prevents.
- The busy-time counter (`mcb_busy_seconds`, the hang overlay after 10 s) and `MCB_HANGLOG` time slices with
  `now_us()`. Proposed: E, so a hang is found the same way every run and a slow Mac is not taken for a hung OS.
  Host-time profiling (`rt.usb_us`, `rt.idle_us`) stays on host time.

## Status, 2026-10-29: parked after phase 2

- **Phase 0 done** (on `main`, b3fe498): instructions are counted in Unicorn's block-start helper, at no
  measurable cost; `MCB_RTLOG` prints them. The OS runs about 110 M instructions a second while playing, 116 M
  idle, about 250 M while booting. K = 250 chosen (the OS is then busy about 44 % of the time).
- **Phase 1 done** (on `main`, 19e7c4b): every hardware model reads `board_time`.
- **Phase 2 built but not merged**: branch `wip/board-clock` (`MCB_WALLCLOCK=1` there runs the old path). It
  works, but fails its own gates:
  - **Host CPU**: about 30 % more than the wall clock for the same instructions (mcrt_test, 4 runs each way:
    ≈43 % against ≈33 %). Not overhead: hooks and services cost a few ms a second. The Mac runs the JIT slower
    when the thread is stopped and put to sleep in the middle of the OS's work every 250 µs (450 against 300 ms
    of host time a second); with an 8 ms lead it matched the wall clock. Sleeping only once the OS is idle helped
    some, not enough.
  - **Lateness**: some runs fall 10–130 ms behind once or twice (one host slice of ~10 ms, mostly in USB
    microframe code; not yet known whether the thread was descheduled or the code was slow).
  - **Boot**: the OS is 100 % busy for the first second, so at K = 250 the Mac must run 250 M instructions a
    second to keep up, and only just does (18–20 ms behind).
- **To pick it up again**: run ahead by an IO period (phase 6's idea) is the likely fix for the CPU cost; measure
  thread CPU time against host time for the long slices first.
