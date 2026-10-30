# Performance findings — 2026-10-29

Research only: no project code was changed. Experiments ran on throwaway copies in a scratch folder.

**Goal (confirmed):** lower CPU **while playing**, and **battery life**, with no audible change. Build time, memory
and launch time are secondary and were not looked at in depth.

Battery depends on two things: how much work the CPU does (CPU %) and how often the Mac's cores are woken up. A core
that is woken thousands of times a second can't drop into its deep low-power state even when it does little each
time. So this report ranks fixes by both.

## Results after implementing R0, F1, F2, F4, F6, F8 (same day)

Measured with `mcrt_test` (it now prints energy, cycles and wakeups per phase from the kernel's own counters, no
sudo), the old and new board run alternately for 3 rounds, 8 s per phase, on the same Mac:

| While playing | Before | After | Change |
|---|---|---|---|
| CPU, USB audio off | 21–22 % | 13 % | **−40 %** |
| Host cycles | 0.91–0.93 G/s | 0.56–0.57 G/s | −39 % |
| Energy (kernel estimate) | 1.69–1.73 W | 0.95–1.00 W | **−43 %** |
| CPU, USB audio on | 25 % | 15 % | −40 % |
| Context switches, USB audio on | ~12,100/s | ~6,300/s | **−48 %** (the 8 kHz watchdog is gone) |
| Context switches, USB audio off | ~6,700/s | ~6,400/s | −5 % |
| Late or repeated audio blocks | 0 | 0 | same |

Whole app, idle, window in background: **~27 % → 13.8 %** of a core (`ps -o time=` over 30 s).

What changed, and how it was checked:

| Item | Change | Check |
|---|---|---|
| R0 | `mcrt_test` prints mW, G cycles/s, context switches, wakeups per phase (`proc_pid_rusage`) | the numbers above |
| F4 | `now_us()` = `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`; MMIO writes read the clock only where needed | `mctiming_test` |
| F1 | DMA reads and writes DDR/SRAM directly (`ram_ptr`). The old `uc_mem_write` path never dropped translated code either (Unicorn's `invalidate_and_set_dirty` is empty), so behaviour is unchanged | `mcunit_test`, `mcusb_test` (bit-exact USB audio), `mcrt_test` |
| F2 | MAC translated as TCG ops for the block's MACSR mode; a MACSR write ends the block. In `patch_unicorn.py` (new last step), vendored files and `unicorn-2.1.4.patch` regenerated from stock 2.1.4 | new `mcemac_test`: 1,000,000 random MACs (register and load forms, all modes, MACSR changed mid-block) equal the old helper; it fails 11 % of cases with a deliberately wrong shift |
| F8 | No watchdog thread: each slice gets an instruction budget of ~1 ms (125 µs with USB audio) at the speed the Mac has been running the OS (learnt as it goes) | `mcunit_test` hang checks, `mcrt_test`, `mcusb_test`, `mcshutdown_test` |
| F6 | No 1 kHz MIDI timer: the board calls back when the OS queues MIDI OUT bytes (`mcb_on_midi_out`); input the board had no room for is retried every 1 ms only while it waits | new Swift test; the rest of `MidiPortPairTests` |

`./build.sh` and `./test.sh` pass (34 Swift tests, `mctiming_test`), as do `mcusb_test` and `mcshutdown_test`.
**Not tried by hand yet:** MIDI out to a real synth or DAW (F6), and an hour on battery. The remaining big battery
item is still the ~6,300 short sleeps a second (see "The bigger battery lever"); F3 and run-ahead are next if you
want more.

## What the app spent its CPU on, before these changes

The app idles at **~27 % of one core** (M4 Pro, steady from 12 s after launch to 72 s). Almost all of it is one
thread, "Model:Cycles CPU", which runs Elektron's OS through the Unicorn JIT. The main thread (SwiftUI) takes
~2.5 %, and the 1 kHz MIDI pump ~0.5 %. So the gains are in the board (`src/mcboard.c`) and in the vendored
Unicorn (`vendor/unicorn`), not in the Swift app.

While the CPU thread is awake, its time splits roughly like this (headless profile while playing, 2,367 busy
samples):

| Where | Share of busy time | What it is |
|---|---|---|
| The OS's own code, translated | 15 % | The real work. Can't shrink without changing the JIT. |
| `helper_cfmac` (+ `_get`) | 13 % | The ColdFire multiply-accumulate instruction, done as a C call each time (the OS's audio maths) |
| Interrupt entry and exit | ~17 % | `helper_check_exit_request` → state restore (10 %), `m68k_cpu_exec_interrupt` (7 %) |
| `dma_run` | 10 % | The board's DMA engine copying memory 4 bytes at a time through Unicorn's API |
| `clock_gettime` | 6 % | Reading the Mac's clock, through a slow path |
| Unicorn's slow memory path | ~15 % | `load/store_helper` → `find_memory_mapping` / `flatview_translate` on MMIO, interrupt stack frames, and DMA |
| everything else | ~24 % | spread thin (TB lookup, MMIO models, USB, audio FIFO) |

The rest of the time, about 60 % of wall time, the thread sleeps in `nanosleep` while the OS idles.

## Baseline (how to rerun it)

All on an M4 Pro (12 cores), macOS 27. The machine was **not idle**: load average ~4, WindowServer ~59 %, Claude
helpers ~25 %. Treat the absolute numbers as ±5 points.

| Measurement | Command | Result |
|---|---|---|
| Board alone, playing | `MCB_RTLOG=1 build/bin/mcrt_test build/main_os.bin <copy of flash.bin> <scratch emmc.img> 15` | 24 % off / 28 % USB audio on / 24 % off again; 0 late blocks |
| Same, 2 more rounds (10 s phases) | same, 10 | 27/31/27 and 22/25/22: **the same build varies by 5 points between runs** |
| Board activity | `MCB_RTLOG` lines | ~5,000 slices/s, ~110 M guest instructions/s, slept ~600 ms of every second |
| Profile while playing | `sample <pid> 10` during phase 1 of `mcrt_test` | table above |
| Whole app, idle, window in background | `open -n -g --env SPOKE_DATA=<scratch copy> --env SPOKE_TEST="75:quit" "build/Vladiktron Spoke.app"`, then `ps -o time=` every 5 s | 46 % in the first 5 s (boot), then 25–29 % flat |
| App threads | `ps -M -p <pid>` | CPU thread ~30 % instantaneous, main thread ~2.5 %, MIDI pump ~0.5 % |
| Wakeups and energy, board playing | `top -l 6 -s 2 -pid <mcrt_test pid> -stats pid,cpu,idlew,power,csw` during phase 1 | **~7,000 context switches/s**, ~280 package idle wakeups/s, Energy Impact 30–43 |

Two measurement lessons that shape everything below:

1. **Whole-process CPU % is too noisy to see a 1–3 point gain.** Two of the experiments below were invisible in it.
   Each fix needs a quieter metric first (see R0).
2. **`sample` changes what it measures in the app.** Sampling the running app pushed its CPU thread to ~95 % busy:
   the pauses make the board fall behind, and it then catches up. App profiles are good for *what* runs, not for
   *how much*. `mcrt_test` under `sample` stayed at ~40 % busy, so it is the better profile.

## Findings

Impact is the share of the CPU thread's busy time a fix could remove; the idle app at ~27 % means 10 % of busy time
≈ 1–2 points of a core. Effort: S < 1 h, M ≈ half a day, L = days.

| # | Issue | Location | Impact | Effort | Risk | Confidence |
|---|---|---|---|---|---|---|
| F1 | DMA copies go 4 bytes at a time through `uc_mem_read`/`uc_mem_write` | `src/mcboard.c:976-1066` (`bus_read`, `bus_write`, `dma_run`) | **High** (10 % busy playing; the biggest item at boot) | S–M | Medium | High (measured) |
| F2 | The ColdFire MAC is an out-of-line helper that forces TCG to spill registers | `vendor/unicorn/qemu/target/m68k/op_helper.c:1005`, `helper.h` (`DEF_HELPER_4(cfmac…)`), `translate.c:5758` | **High** (13 % busy) | M | Medium | High on cost, medium on the gain |
| F3 | Every interrupt exits through a full TB state restore; the vector fetch takes the slow load path | `vendor/unicorn/qemu/accel/tcg/tcg-runtime.c` (`HELPER(check_exit_request)`), m68k `do_interrupt_all` | Medium (~17 % busy is interrupt overhead; maybe half removable) | M–L | Medium–High | Medium |
| F4 | `now_us()` uses `CLOCK_MONOTONIC`, which on macOS takes a slow `gettimeofday` path; MMIO writes read the clock even when they don't need it | `src/mcboard.c:62-66`, `:754` | Low–Medium (6 % busy) | S | Low | High on cost; gain not visible in whole-process CPU |
| F5 | Unicorn looks up the memory region on every slow-path access | `vendor/unicorn/qemu/accel/tcg/cputlb.c` (`load_helper`, `store_helper`: `uc->memory_mapping`) | Low–Medium | M | Medium | Medium |
| F6 | 1 kHz MIDI pump timer runs even with nothing connected | `app/MidiPort.swift:133-136` | Low (~0.5 % of a core) | S | Low | Medium |
| F8 | A watchdog thread wakes 1,000×/s (8,000×/s with USB audio) just to end CPU slices, even while the board sleeps | `src/mcboard.c:1622-1629` (`watchdog_main`) | Medium for battery (~1/7 of all wakeups), low for CPU % | M | Medium | Medium |
| F7 | Every LED or screen change re-evaluates the whole `PanelView` | `app/PanelView.swift:83`, `app/Board.swift:41-42` | Unknown, probably low | S–M | Low | Low (not measured while playing) |
| N1 | *Not a problem:* the 12 kHz panel scan | `src/mcboard.c:284-297` | — | — | — | Medium |

### F1: DMA copies through the slowest API, 4 bytes at a time

**Evidence.** The OS uses the eDMA to copy audio mixer buffers and, through eSDHC, the +Drive's data. `dma_run`
moves each unit with `bus_read` → `uc_mem_read` and `bus_write` → `uc_mem_write`, 4 bytes per call. Each call
goes through Unicorn's address-space lookup (`find_memory_mapping` → `flatview_translate` →
`address_space_translate_internal`) and then `flatview_read/write`. Profile while playing: **233 of 2,367 busy
samples (10 %)**, more than half of it in those lookups. In the app profile (boot and its catch-up) `dma_run` is
**1,581 of 4,350 samples (36 %)**, the largest single item. So it matters most at boot and when the board is
catching up.

**Fix.** DDR is `b->ddr` (mapped at 0x40000000 and mirrored at 0x48000000), and SRAM is `b->sram`. In `dma_run`,
when source and destination are plain RAM, the sizes are 4/16/32 bytes and there's no modulo, copy straight between
host pointers:

```c
static uint8_t *ram_ptr(mcboard *b, uint32_t a, uint32_t n) {        // NULL: not plain RAM, use the bus
    if (a >= 0x40000000u && a < 0x50000000u && ((a & (DDR_SIZE - 1)) + n) <= DDR_SIZE) return b->ddr + (a & (DDR_SIZE - 1));
    if (a >= 0x80000000u && a + n <= 0x80010000u) return b->sram + (a - 0x80000000u);
    return NULL;
}
// in the minor loop: if (!smod && !dmod && soff == ssz && doff == dsz && (s = ram_ptr(..)) && (d = ram_ptr(..))
//                       && !uc_ram_page_has_code(...dest pages...)) { memmove(d, s, nbytes); sa += nbytes; da += nbytes; }
```

Guest memory in `b->ddr` is already big-endian bytes, so a `memmove` keeps the byte order, just as the current
byte-wise path does. **Risk:** a direct write skips Unicorn's translated-code invalidation. The project's Unicorn
patch already exports `uc_ram_page_has_code` (`vendor/unicorn-2.1.4.patch:181`). Fall back to the slow path when a
destination page holds code. Keep the slow path for MMIO (MIDI channels 36/37 read the UART).

**Verify.** `mcunit_test` (add a check: a DDR→DDR TCD gives the same bytes and the same final SADDR/DADDR/CITER as
before), `mcusb_test` (bit-exact USB audio), `mcshutdown_test`, `mctiming_test`. Then the R0 busy metric while
playing, and boot time to a ready screen (`mcboot`, time to first `IDLE_PC`).

### F2: ColdFire MAC is a C call per instruction

**Evidence.** `helper_cfmac` is the top non-JIT function: **310 of 2,367 busy samples (13 %)** playing, and 1,238
of 4,350 (28 %) in the app profile. It's declared `DEF_HELPER_4(cfmac, void, env, i32, i32, i32)` with no flags,
so TCG assumes it may read and write every guest register. Before each MAC it writes all live registers back to
`env`, and after it reloads them. The OS's audio code runs MACs back to back, so that spill and reload repeats in
the tightest loop. Inside, it also does 128-bit maths and rereads `macsr` on every call.

**Fix, in two steps.**
1. Declare it `DEF_HELPER_FLAGS_4(cfmac, TCG_CALL_NO_WG, …)`: it writes only `env->macc[]`, which isn't a TCG
   global (check that `macc` has no `tcg_global_mem_new` in `translate.c`). `NO_WG` still allows reading
   globals such as `MACSR`. That removes the reload after each call.
2. Specialise at translate time: `f` is a translate-time constant, so emit separate helpers, or inline TCG
   (`muls2`/`ext`/`add_i64` into `env->macc`), for the common word/long, integer/fractional forms, using 64-bit
   maths where the product can't overflow.

**Verify.** It's a pure function, so first write a C test that compares the old and new helpers on several million
random `(rx, ry, f, macsr)` inputs. Then capture the main output for a fixed pattern before and after with
`mcaudio_test` and compare. Exact sample equality is not guaranteed between wall-clock runs, so compare levels and
spectra, or add a deterministic capture once CLOCK-PLAN lands. Finally the R0 metric.

### F3: Interrupt entry and exit are expensive

**Evidence.** RELEASE.md ("Less work per interrupt") already noted part of this. Measured now:
`helper_check_exit_request` takes 227 busy samples (10 %), mostly `cpu_loop_exit_restore` → `cpu_restore_state`
(a TB lookup and re-decode). `m68k_cpu_exec_interrupt` takes 160 (7 %), of which `do_interrupt_all` fetches the
vector with `cpu_ldl_mmuidx_ra` through the slow load path (109) and pushes the frame through `store_helper` with a
region lookup (65). The OS takes about 15,000 interrupts a second (12 kHz panel scan, 1 kHz key handler, 1,500
audio blocks, timers), and each one pays this.

**Fix ideas, not yet proven.**
- The exit check runs at the start of a TB, where the guest PC is `tb->pc` and m68k's `cc_op` is still dynamic. Pass
  `tb->pc` into the helper, store it, and use `cpu_loop_exit` instead of `cpu_loop_exit_restore`, so no restore is
  needed.
- Read the vector table directly from host memory (VBR points at RAM) instead of through `cpu_ldl`.

**Risk:** this is the part of Unicorn where earlier patches already found subtle bugs (the SR read, lazy condition
codes). **Verify** with the whole test set, plus a long scripted app run with `MCB_HANGLOG=1` that checks for zero
OS exceptions, then the R0 metric.

### F4: Reading the clock takes a slow path

**Evidence.** On macOS, `clock_gettime(CLOCK_MONOTONIC)` goes `_mach_boottime_usec` → `gettimeofday` →
`mach_absolute_time`. That's **145 busy samples (6 %)**: 82 from `mcb_run`'s loop (several reads per slice) and 56
from `mmio_write`. `mmio_write` calls `board_time(b)` at `mcboard.c:754` on *every* MMIO write, before it knows
whether the write needs the time.

**Experiment.** A scratch build with `now_us()` = `clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-3` scored 27/29/24
and 23/27/23, against the normal build's 27/31/27 and 22/25/22. That's within noise, so not visible this way.

**Fix.** That one-line change, plus computing `t` only in the branches that use it. `CLOCK_UPTIME_RAW` also stops
while the Mac sleeps, so the board no longer sees a jump of minutes on wake (today the 20 ms resync handles it).
Check that's what you want. **Verify** with the R0 metric and `mctiming_test`.

### F5: Unicorn's slow memory path looks up the region every time

**Evidence.** Unicorn's `load_helper` and `store_helper` call `uc->memory_mapping(uc, paddr)` on every slow-path
access, even with no memory hooks to run: `find_memory_mapping` + `flatview_translate` +
`address_space_translate_internal` add up to ~470 busy samples across F1, F3 and MMIO. F1 and F3 remove most of
what reaches this path. What's left is MMIO, which has to take it. **Fix only if needed after F1/F3:** cache the
last region in `find_memory_mapping`.

### F6: MIDI pump wakes 1,000 times a second regardless

`MidiPort.start` schedules a 1 ms `DispatchSourceTimer` that pumps both port pairs whether or not anything is
connected or queued. `ps -M` showed that thread at ~0.5–0.7 % of a core. **Fix:** pump only while a destination
is connected or the board has output waiting, or slow to ~5 ms while idle. Clock-out timing (1 ms) only matters when
someone listens. **Verify:** `ps -M`, plus MIDI clock jitter with a DAW (RELEASE.md's MIDI items).

### F7: Panel re-evaluates on every LED or screen change (not measured)

`PanelView` is the only `@ObservedObject` of `Board`. Any change to `leds` or `screen` rebuilds the whole panel
body (≈80 views with gradients and shadows). Idle, the main thread is ~2.5 %, so this is small there. While playing,
LEDs and the screen change up to ~30 times a second, and that wasn't measured (it would play sound on your
speakers). **Fix if it shows up:** move `leds` and `screen` into small observable objects watched only by the LED,
pad and screen views. **Verify:** `ps -M` main-thread CPU while playing, before and after.

### F8: The watchdog thread wakes up 1,000 times a second for nothing, most of the time

**Evidence.** While playing, the board process does about 7,000 context switches a second. Roughly 5,000 come from
the CPU thread's 125–250 µs sleeps, which are needed with today's design (longer sleeps were measured to cost
*more*, mcboard.c:47-53). Another ~1,000 come from `watchdog_main`. It `usleep`s 1 ms (125 µs while USB audio
streams) and kicks Unicorn to end the slice. It wakes on schedule even when the CPU thread is asleep and there's
nothing to kick. That's about 60 % of the time while playing.

**Fix.** End slices without a second thread. The instruction limit from CLOCK-PLAN phase 0 is already on main
(`uc_m68k_set_ilimit`, mcboard.c:19) but isn't used for slicing. Set it to about one slice's worth of
instructions (~110 per µs measured, so ~110,000 for 1 ms) before each `uc_emu_start`, and stop the watchdog
thread. This is *not* the parked phase 2: the board still runs on the Mac's clock and sleeps exactly as now. Only
what ends a slice changes. **Risk:** a guest stuck with interrupts masked must still end its slice; the limit does
that too. Keep a slow (100 ms) watchdog only as a safety net if wanted.

**Verify.** `top -stats csw,idlew,power` before and after (expect ~1,000 fewer switches/s), and `mcrt_test` (0 late
blocks, 48 kHz). With USB audio on, check `mcusb_test`, since its 125 µs slices must stay the same length.

### The bigger battery lever (not a quick fix)

The ~5,000 sleeps a second are the real battery cost, and no fix above removes them. They exist because the board
follows the Mac's clock in tiny steps. The way to wake a few hundred times a second instead is CLOCK-PLAN's "run
ahead by one audio buffer": render a few milliseconds of board time in one go, then sleep until the audio device
needs more. That is the parked `wip/board-clock` work. Its +30 % CPU came from sleeping in the middle of work,
which run-ahead avoids. F1–F3 make each burst cheaper, so they help that plan rather than compete with it. Measure
with `powermetrics` on battery (it needs `sudo`, so run it yourself) before and after.

### N1: The 12 kHz panel scan is not worth touching (negative result)

A scratch build with PIT3 slowed to 3 kHz scored 23/26/22 and 25/29/27, again within noise of the normal build.
Since it would also change how keys, encoders, pads and LEDs behave, **don't pursue it**. The interrupt cost is
better cut per interrupt (F3) than by having fewer of them.

## Recommended order

0. **R0, measurement first (S).** Add a busy metric that isn't wall-clock CPU %: the CPU thread's own CPU time
   (`clock_gettime(CLOCK_THREAD_CPUTIME_ID)` in `mcb_run`) per second of board time, printed by `MCB_RTLOG` and
   `mcrt_test`. Also record guest instructions per CPU-second. Run it 3× per build, back to back, on a quiet Mac.
   Without this, F2–F4 can't be shown to help.
Ranked for **CPU while playing** and **battery**. Also add context switches/s (`top -stats csw`) to R0, since
battery depends on wakeups as well as CPU %.

1. **F4 (S):** the clock change. Trivial and safe; it proves R0 can see a ~6 % change.
2. **F2 (M):** the MAC helper. It's the OS's audio maths, so it matters most while playing. `TCG_CALL_NO_WG` first
   (one line, tested against the old helper), then specialise.
3. **F1 (S–M):** the DMA fast path. Mixer copies run on every audio block while playing; helps boot too.
4. **F8 (M):** slices ended by the instruction limit, no watchdog thread: ~1,000 fewer wakeups a second.
5. **F6 (S):** idle MIDI pump: another ~1,000 wakeups a second when nothing is connected.
6. **F7:** measure the main thread while playing (command under Open questions). Fix only if it's more than a few %.
7. **F3 (M–L):** interrupt exit without the restore. The largest remaining CPU item, and the riskiest. Do it on a
   branch.
8. **Run-ahead (L):** the real battery fix (see "The bigger battery lever"). Plan it after 1–5, building on
   `wip/board-clock`.
9. F5 only if a measurement after these points at it.

If 1–3 deliver their profile shares, the CPU thread's busy time drops by roughly a fifth to a quarter. At ~27 %
for the whole app that's on the order of **4–7 points of a core**, an estimate from profile shares and not a
measurement.

## Open questions

- ~~What matters most~~ Answered: CPU while playing and battery. The order above reflects that.
- **Battery numbers on battery power:** `sudo powermetrics --samplers tasks --show-process-energy -i 5000` while
  the app plays, unplugged. That needs your password, so I couldn't run it.
- **System sleep:** with `CLOCK_UPTIME_RAW` (F4) the board's clock pauses while the Mac sleeps. Is that what you
  want, or should a wake still resync?
- **Playing in the full app** wasn't measured: it plays sound on your speakers, so I left it to you. The command:
  `open -n --env SPOKE_DATA=<copy> --env SPOKE_TEST="8:play,60:quit" "build/Vladiktron Spoke.app"`, then
  `ps -M` every few seconds.
- **Build and test time** were not investigated (AUDIT.md measured `build.sh` at 5.6 s). Say if they matter.
- **How F1–F3 interact with CLOCK-PLAN phase 2** (`wip/board-clock`): that branch's extra CPU came from sleeping
  mid-work. Cheaper per-interrupt and per-copy work should help it too, but that's untested.
