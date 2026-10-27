// Model:Cycles virtual board. See mcboard.h. Addresses are MCF5441x / OS 1.13; the notes next to each
// model say which OS routine relies on it.
#include "mcboard.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <unicorn/unicorn.h>

void uc_m68k_set_irq(uc_engine *uc, int level, int vector);   // patched Unicorn (patch_unicorn.py)
void uc_m68k_kick(uc_engine *uc);

#define DDR_SIZE 0x08000000u
#define OS_BASE 0x40000400u
#define OS_ENTRY 0x400004e8u
#define SCREEN_PTR 0x401492f4u
#define EMMC_SECTORS 3817472u        // Samsung KLM4G1FETE in pSLC mode, as factory-configured units are
                                     // (OS table 0x40149264, entry 4: 7634944 raw, 3817472 once in SLC mode)
#define SLICE_US 1000                // CPU runs in 1 ms slices; timers and lines are serviced between them
#define SLICE_USB_US 125             // ... and in 125 us slices (one USB microframe) while USB audio streams
#define USB_WAIT_US 500              // longest an audio block waits for earlier USB microframes (dma50_check)
#define AFIFO 16384                  // audio FIFO, frames (power of two)
#define BLOCK_US (32 * 1e6 / 48000)  // one 32-frame audio block
#define MIDI_RING 4096               // MIDI byte rings to/from the host (power of two)
#define MIDI_BYTE_US 320.0           // one byte at 31250 baud (10 bits)
#define UART9 0xEC074000u            // MIDI IN / OUT
#define USB 0xFC0B0000u              // USB OTG controller, device mode (ChipIdea/EHCI style)
// Power: the power key is port A pin data (0xEC094018) bit 2, 1 = pressed. (Bit 3 next to it is the battery
// sense, 1 = on battery, read at 0x40053242; it stays 0, on mains, so the OS never takes its low-battery
// shutdown.) The OS polls the key at 0x400538a2 (a timer task set up at 0x40005558). Once it has seen the key
// released and then held for 13 polls (threshold set at 0x40007f7c, kept at 0x4013e760), it shows "Shutting
// down..." and calls its power-off routine 0x400e1e0e. That runs the registered shutdown handlers (table
// 0x40a74578; they save the working state to the +Drive), masks interrupts, clears port C bit 2 (the power
// latch) and spins at 0x400e1e38 for good.
#define PORTA_POWER_KEY 0x04
#define POWER_OFF_SPIN 0x400e1e38u
// Idle: once started, the OS does all its work in interrupts (timers, audio render, USB, MIDI); the boot code
// ends in a branch to itself at IDLE_PC. When the CPU gets there, nothing runs until the next interrupt, so the
// CPU thread sleeps until the next timer, audio block, USB microframe or MIDI byte is due, instead of spinning.
// Sleeps are at least IDLE_MIN_US (wakeups cost host CPU) and at most IDLE_MAX_US (one microframe while USB
// audio streams); input from other threads is noticed then. Short sleeps cost less than long ones: after a long
// one the 12 kHz panel scan and the audio blocks pile up, and catching up costs more host CPU than it saved
// (measured with mcrt_test: 125/250 us took 23 % CPU, 250/1000 us 48 % and blocks up to 5 ms late).
#define IDLE_PC 0x400057e6u
#define IDLE_MIN_US 125
#define IDLE_MAX_US 250

// ---------------------------------------------------------------- small helpers
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3;
}
static void swap32_copy(uint8_t *dst, const uint8_t *src, size_t n) {   // eSDHC little-endian transfer mode
    for (size_t i = 0; i + 3 < n; i += 4) {
        dst[i] = src[i + 3]; dst[i + 1] = src[i + 2]; dst[i + 2] = src[i + 1]; dst[i + 3] = src[i];
    }
}

// ---------------------------------------------------------------- board state
typedef struct {
    uint8_t *mem;
    size_t size;
    const char *path;
    int wel;
    int cmd, nargs, dirty;
    uint8_t args[4];
    uint32_t addr;
} spiflash;

typedef struct {
    FILE *f;
    char ext_path[1024];
    uint8_t ext[512];
    uint8_t bus_test[8];
    int bus_test_len;
} emmc;

struct mcboard {
    uc_engine *uc;
    uint8_t *ddr, *sram, *scratch;
    uint8_t *io_ec, *io_fc;                 // register files for 0xEC000000 / 0xFC000000 (16 MB each)
    double t0;                              // host time at boot (us)

    // interrupt controllers: asserted lines, masks
    uint64_t line[3], imr[3], force[3];     // device lines, masks, software-forced sources (INTFRCH/L)
    int irq_level, irq_vector;

    // timers
    double pit_due[4], pit_period[4];
    double dtim_due[4], dtim_period[4];

    // GPIO outputs
    uint8_t gpio_out[12];

    // front panel on FlexBus 0x8C000000 (74HC373 latches): key columns are active-low
    uint8_t keys[8];                        // raw key bits per column, 1 = pressed (0x4007fbde)
    atomic_int enc_pending[16];             // knob turns still to deliver, in quadrature transitions (raw index)
    uint8_t enc_state[16];                  // current 2-bit Gray state per raw encoder
    uint8_t enc_shown[4];                   // encoder byte last presented per column 4..7
    atomic_int pad_level[7];                // pad pressure as ADC value, by pad 1..6
    int fp_col;
    uint64_t n_fp_reads, n_irq_taken;
    uint16_t fp_latch[8];                   // last values written to 0x8C000000 + 2*i (LED drive)

    // SPI flash on DSPI0
    spiflash flash;
    uint8_t dspi0_rx[64];
    int dspi0_n;

    // eSDHC + eMMC + eDMA ch59
    emmc card;
    uint32_t sd_irq, sd_rsp[4];
    uint32_t sd_rbuf[128];
    int sd_rn, sd_rpos;
    uint8_t sd_wbuf[1024];
    int sd_wneed, sd_wn, sd_wcmd;
    uint32_t sd_warg;
    int dma59_enabled;

    // audio: eDMA channel 50 streams a 64-frame stereo ring (int32 L,R) to SSI0 at 48 kHz, interrupting at
    // each half (0x400442ba). We play it by the host clock and hand each half to Core Audio through a FIFO.
    int dma50_enabled;
    double dma50_due;                       // host time (us) of the next 32-frame boundary
    int dma50_half;                         // half the DMA plays next
    double dma50_stamp;                     // scheduled time (us) of the block just started
    uint32_t dma50_base;                    // ring start (SADDR at enable)
    float *afifo;                           // stereo float ring for the app (AFIFO frames)
    atomic_uint_fast64_t afifo_w, afifo_r;
    uint64_t n_blocks;
    // MCB_RTLOG: real-time health, logged each second (rt_log)
    struct {
        double next, late_max;              // next log, latest block (us after it was due) this second
        uint64_t slices, late, resync, wait_irq, wait_render, wait_usb, sof_irq, dtim2;
        double usb_us;                      // host time spent on microframes
        double idle_us;                     // host time the CPU thread slept, idle
        uint64_t blocks0, uframes0, skips0;   // totals at the last log
    } rt;

    // audio codec on I2C0 (address 0x1A, sent as 0x34): register writes are (reg, value) pairs
    uint8_t codec[256];
    int i2c_phase, i2c_reg;
    uint64_t codec_writes;
    uint64_t dma_int;                       // eDMA INT bits (channel 59 used)
    uint64_t erq;                           // eDMA request enables (SERQ/CERQ), for the MIDI channels 36/37

    // MIDI on UART9: eDMA channel 36 moves each received byte (URB) into a 32 KB ring and interrupts
    // (0x40001094); channel 37 sends whole messages from a 4 KB ring to UTB (0x40001100, done: 0x40001000);
    // single bytes go straight to UTB or, while the DMA is busy, through the UART interrupt (0x40000e8c,
    // INTC1 source 53). Our UART transmits at once, so TxRDY is always set.
    uint8_t uart_rb, uart_imr;
    uint8_t midi_in[MIDI_RING], midi_out[MIDI_RING];
    atomic_uint_fast64_t min_w, min_r, mout_w, mout_r;
    double midi_rx_due;
    uint64_t n_midi_in, n_midi_out;

    // USB: the controller (registers at USB + 0x140.., dQHs at EPLISTADDR = SRAM 0x80000000, dTDs in memory) and
    // a built-in host on the other end of the cable, which enumerates the device and then talks USB-MIDI.
    struct {
        uint32_t cmd, sts, intr, devaddr, eplist, portsc, otgsc, setupstat, epsr, complete;
        uint32_t cur[8];                    // dTD the controller is working on, per endpoint (ep*2 + IN)
        int vbus, connect;                  // cable plugged (VBUS) / the host should talk to us
        double t_run;                       // host time the device started running (FRINDEX base)
        int state;                          // usb_host_state
        double due;                         // next host action
        int step, ctl_phase, ctl_in;        // enumeration script position, control transfer phase
        uint16_t ctl_len;
        uint8_t ctl_buf[1024];
        int ctl_got, tries;
        uint8_t config_value;
        int midi_out_ep, midi_in_ep;        // bulk endpoints of the MIDIStreaming interface (host view)
        // USB audio (UAC2, 48 kHz, 32-bit stereo): capture = Model:Cycles -> host (iso IN), playback = host ->
        // Model:Cycles (iso OUT, paced by the feedback endpoint). One packet each per 125 us microframe.
        int cap_ep, cap_iface, play_ep, play_iface, fb_ep, cap_alt, play_alt, cap_on, play_on;
        atomic_int want_cap, want_play;     // what the app asked for (SET_INTERFACE alt 1 / 0)
        int req_iface, req_alt;             // SET_INTERFACE in flight (READY state)
        int restart;                        // the bus stalled while streaming: restart the streams
        uint64_t uframe;                    // next microframe; FRINDEX shows the last one sent
        double fb, play_acc;                // feedback (frames per microframe), fractional frames owed
        float *cap, *play;                  // stereo float rings (AFIFO frames each)
        atomic_uint_fast64_t cap_w, cap_r, play_w, play_r;
        uint64_t n_cap, n_play, n_play_short, n_uframes, n_cap_empty, n_skips;
        uint32_t gpt_ld[2], gpt_ctrl[2];    // general-purpose timers 0/1 (1 us ticks) -> USBSTS TI0/TI1
        double gpt_due[2];
        uint8_t ev[4096 * 4];               // USB-MIDI event packets waiting for the device (host -> device)
        atomic_uint_fast64_t ev_w, ev_r;
        uint8_t ev_status, ev_data[3], ev_n; // host-side parser of raw bytes into event packets
        uint8_t sx[3]; int sx_n, in_sysex;
        uint8_t out[MIDI_RING];             // raw MIDI bytes from the device
        atomic_uint_fast64_t out_w, out_r;
        uint64_t n_in, n_out;
    } usb;

    // power: key held (thread-safe), the OS has powered off (reached POWER_OFF_SPIN)
    atomic_int power_key, powered_off;

    // run control / outputs
    atomic_int stop;
    atomic_int running;                     // a slice is executing (watchdog kicks it)
    int idle;                               // the slice ended at IDLE_PC
    atomic_uint last_pc, last_sr;           // CPU state after the last slice, for mcb_status: only the CPU
                                            // thread may call into Unicorn while it runs (Unicorn keeps the
                                            // thread's JIT write/execute state in shared fields; a uc_reg_read
                                            // from another thread can leave the CPU thread's JIT pages
                                            // writable, and the next slice faults executing them)
    pthread_t watchdog;
    pthread_mutex_t lock;
    uint8_t screen[1024];
    uint64_t screen_gen;
    double next_screen;
    char fault[256];
    uint64_t n_irq[256], n_cmd, n_rd, n_wr, n_erase, n_emmc_err;
};

static uint8_t *io_ptr(mcboard *b, uint32_t a) {
    return (a >> 24) == 0xEC ? &b->io_ec[a & 0xFFFFFF] : &b->io_fc[a & 0xFFFFFF];
}
static uint32_t io_get(mcboard *b, uint32_t a, int n) {
    uint8_t *p = io_ptr(b, a);
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}
static void io_set(mcboard *b, uint32_t a, int n, uint32_t v) {
    uint8_t *p = io_ptr(b, a);
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

// ---------------------------------------------------------------- interrupt controllers (INTC0..2)
// Vector = 64 + 64*n + source; level = ICRn[source] & 7; IMR set/clear via SIMR (0x1C) / CIMR (0x1D).
static void irq_update(mcboard *b) {
    int best_level = 0, best_vec = 0;
    for (int n = 0; n < 3; n++) {
        uint64_t act = (b->line[n] & ~b->imr[n]) | b->force[n];   // forced sources ignore the mask (the OS never unmasks its forced render interrupt, INTC1 source 63)
        while (act) {
            int src = __builtin_ctzll(act);
            act &= act - 1;
            int level = io_get(b, 0xFC048040u + 0x4000u * n + src, 1) & 7;
            if (level > best_level || (level == best_level && level && 64 + 64 * n + src > best_vec)) {
                best_level = level;
                best_vec = 64 + 64 * n + src;
            }
        }
    }
    if (best_level != b->irq_level || best_vec != b->irq_vector) {
        b->irq_level = best_level;
        b->irq_vector = best_vec;
        uc_m68k_set_irq(b->uc, best_level, best_vec);
    }
}
static void set_line(mcboard *b, int intc, int src, int on) {
    uint64_t bit = 1ull << src;
    uint64_t old = b->line[intc];
    b->line[intc] = on ? old | bit : old & ~bit;
    if (b->line[intc] != old) irq_update(b);
}

// ---------------------------------------------------------------- timers
// PITs: 135.168 MHz (the OS programs PIT0 100 Hz, PIT2 120 Hz, PIT3 12 kHz with it). PIT n -> INTC2 source 13+n.
// PCSR: EN 0, RLD 1, PIF 2 (w1c), PIE 3, PRE 11:8. Without PIE (busy-wait delays) PIF reads as set.
#define PIT_CLOCK_MHZ 135.168
static void pit_program(mcboard *b, int n, double t) {
    uint32_t base = 0xFC080000u + 0x4000u * n;
    uint32_t pcsr = io_get(b, base, 2), pmr = io_get(b, base + 2, 2);
    if ((pcsr & 1) && (pcsr & 8)) {
        b->pit_period[n] = (pmr + 1.0) * (double)(1u << ((pcsr >> 8) & 15)) / PIT_CLOCK_MHZ;
        if (b->pit_due[n] == 0) b->pit_due[n] = t + b->pit_period[n];
    } else {
        b->pit_due[n] = 0;
        set_line(b, 2, 13 + n, 0);
    }
}
// DMA timers n -> INTC0 source 32+n. DTMR: RST 0, CLK 2:1, FRR 3, ORRI 4, PS 15:8; DTRR reference; DTER w1c.
#define DTIM_CLOCK_MHZ 135.168
static void dtim_program(mcboard *b, int n, double t) {
    uint32_t base = 0xFC070000u + 0x4000u * n;
    uint32_t mode = io_get(b, base, 2), ref = io_get(b, base + 4, 4);
    if ((mode & 1) && (mode & 0x10)) {
        double rate = DTIM_CLOCK_MHZ / (((mode >> 8) & 0xFF) + 1);
        b->dtim_period[n] = (mode & 8) ? (ref + 1.0) / rate : 0;
        b->dtim_due[n] = t + (ref + 1.0) / rate;
    } else {
        b->dtim_due[n] = 0;
    }
}
// Counter value: the clock DTMR selects (CLK 2:1 = 1 internal, 2 internal / 16, 3 the DTIN pin, which on this
// board is the 12.288 MHz audio master clock: timer 2, whose ticks the OS turns into frames as (t + 128) >> 8),
// divided by PS + 1.
#define AUDIO_MCLK_MHZ 12.288
// The OS time-stamps two events with timer 2: the audio block (DMA interrupt 0x400589c0, stored at 0x40a78e04)
// and the USB SOF (0x40004318, which keeps "SOF - block stamp", i.e. how far into the block it fell: the USB
// audio stream is aligned and paced by that). Our interrupts run up to a CPU slice late and in either order, so
// the block read returns when the block was due, and the SOF read returns when the microframe was due: while
// streams run, microframes and blocks go in time order (dma50_check, usb_microframes), so the two agree.
static uint32_t dtim_count(mcboard *b, int n) {
    uint32_t mode = io_get(b, 0xFC070000u + 0x4000u * (uint32_t)n, 2);
    int clk = (mode >> 1) & 3;
    double mhz = clk == 3 ? AUDIO_MCLK_MHZ : clk == 2 ? DTIM_CLOCK_MHZ / 16 : DTIM_CLOCK_MHZ;
    double t = now_us() - b->t0;
    if (n == 2) {
        uint32_t pc;
        b->rt.dtim2++;
        uc_reg_read(b->uc, UC_M68K_REG_PC, &pc);
        if (pc >= 0x400589c0u && pc < 0x40058a0au && b->dma50_stamp > 0) t = b->dma50_stamp;
        else if (pc >= 0x40004318u && pc < 0x400043fcu && b->usb.uframe) t = b->usb.t_run + 125.0 * (double)(b->usb.uframe - 1);
    }
    return (uint32_t)(t * mhz / (((mode >> 8) & 0xFF) + 1));
}
// A due PIT fires (PIF + line). Called between slices and again whenever the handler acknowledges the timer,
// so fast timers (the 12 kHz panel scan) keep their real rate inside a 1 ms slice; at most 5 ms of backlog.
static void pit_check(mcboard *b, int n, double t) {
    if (!b->pit_due[n] || t < b->pit_due[n]) return;
    io_set(b, 0xFC080000u + 0x4000u * n, 2, io_get(b, 0xFC080000u + 0x4000u * n, 2) | 4);
    set_line(b, 2, 13 + n, 1);
    b->pit_due[n] += b->pit_period[n];
    if (t - b->pit_due[n] > 5000) b->pit_due[n] = t + b->pit_period[n];
}
static void timers_service(mcboard *b, double t) {
    for (int n = 0; n < 4; n++) {
        pit_check(b, n, t);
        if (b->dtim_due[n] && t >= b->dtim_due[n]) {
            io_set(b, 0xFC070003u + 0x4000u * n, 1, io_get(b, 0xFC070003u + 0x4000u * n, 1) | 2);   // DTER.REF
            set_line(b, 0, 32 + n, 1);
            b->dtim_due[n] = b->dtim_period[n] ? b->dtim_due[n] + b->dtim_period[n] : 0;
        }
    }
}

// ---------------------------------------------------------------- SPI flash (EN25QH16) on DSPI0
static void flash_save(mcboard *b) {                         // whole image, swapped in atomically: a quit or
    if (!b->flash.dirty) return;                             // crash mid-save never leaves a torn flash.bin
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", b->flash.path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(b->flash.mem, 1, b->flash.size, f) == b->flash.size;
    ok &= fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    if (ok && rename(tmp, b->flash.path) == 0) b->flash.dirty = 0;
    else { fprintf(stderr, "[flash] could not save %s\n", b->flash.path); unlink(tmp); }
}
static uint8_t flash_xfer(spiflash *s, uint8_t v) {
    if (s->cmd < 0) {
        s->cmd = v;
        s->nargs = 0;
        if (v == 0x06) s->wel = 1;
        if (v == 0x04) s->wel = 0;
        return 0xFF;
    }
    switch (s->cmd) {
    case 0x9F: { static const uint8_t id[] = {0x1C, 0x70, 0x15}; return s->nargs < 3 ? id[s->nargs++] : 0xFF; }
    case 0x05: return s->wel ? 0x02 : 0x00;
    case 0x03: case 0x0B: case 0x02: case 0x20: case 0xD8:
        if (s->nargs < 3) {
            s->args[s->nargs++] = v;
            if (s->nargs == 3) {
                s->addr = (uint32_t)s->args[0] << 16 | s->args[1] << 8 | s->args[2];
                if (s->cmd == 0x20 && s->wel) { memset(s->mem + (s->addr & ~0xFFFu) % s->size, 0xFF, 0x1000); s->dirty = 1; }
                if (s->cmd == 0xD8 && s->wel) { memset(s->mem + (s->addr & ~0xFFFFu) % s->size, 0xFF, 0x10000); s->dirty = 1; }
            }
            return 0xFF;
        }
        if (s->cmd == 0x0B && s->nargs == 3) { s->nargs++; return 0xFF; }
        if (s->cmd == 0x02) {
            if (s->wel) { s->mem[s->addr % s->size] &= v; s->dirty = 1; }
            s->addr++;
            return 0xFF;
        }
        return s->mem[s->addr++ % s->size];
    }
    return 0xFF;
}

// ---------------------------------------------------------------- eMMC card (Samsung "4FTE4R")
// MCB_EMMCLOG=1 logs every command; storage errors (see emmc_error) are always logged.
static int emmc_log(void) { static int v = -1; if (v < 0) v = getenv("MCB_EMMCLOG") != NULL; return v; }
static void emmc_error(mcboard *b, const char *what, int idx, uint32_t arg) {
    b->n_emmc_err++;
    fprintf(stderr, "[emmc] error: %s (CMD%d, arg %08x)\n", what, idx, arg);
}
static int emmc_in_range(mcboard *b, int idx, uint32_t arg, size_t n) {
    if ((uint64_t)arg + (n + 511) / 512 <= EMMC_SECTORS) return 1;
    emmc_error(b, "beyond the end of the card", idx, arg);
    return 0;
}
static void emmc_save_ext(emmc *c) {
    FILE *f = fopen(c->ext_path, "wb");
    if (f) { fwrite(c->ext, 1, 512, f); fclose(f); }
}
static int emmc_open(emmc *c, const char *path, char *err, size_t errlen) {
    struct stat st;
    if (stat(path, &st) != 0) {
        FILE *f = fopen(path, "wb");
        if (!f) { snprintf(err, errlen, "cannot create %s", path); return -1; }
        fclose(f);
        truncate(path, (off_t)EMMC_SECTORS * 512);          // sparse
    }
    c->f = fopen(path, "r+b");
    if (!c->f) { snprintf(err, errlen, "cannot open %s", path); return -1; }
    snprintf(c->ext_path, sizeof c->ext_path, "%s.extcsd", path);
    FILE *e = fopen(c->ext_path, "rb");
    if (e && fread(c->ext, 1, 512, e) == 512) { fclose(e); return 0; }
    if (e) fclose(e);
    memset(c->ext, 0, 512);
    c->ext[136] = 0;                                         // ENH_START_ADDR = 0
    c->ext[140] = 0xE9;                                      // ENH_SIZE_MULT: whole user area enhanced (pSLC)
    c->ext[155] = 1;                                         // PARTITION_SETTING_COMPLETED (else "MMC not in SLC mode")
    c->ext[156] = 1;                                         // PARTITIONS_ATTRIBUTE: ENH_USR
    c->ext[157] = 0xE9;                                      // MAX_ENH_SIZE_MULT
    c->ext[160] = 7;                                         // PARTITIONING_SUPPORT
    c->ext[192] = 8;                                         // EXT_CSD_REV 5.1
    c->ext[194] = 2;
    c->ext[196] = 0x57;                                      // CARD_TYPE
    c->ext[212] = EMMC_SECTORS & 0xFF; c->ext[213] = EMMC_SECTORS >> 8 & 0xFF;
    c->ext[214] = EMMC_SECTORS >> 16 & 0xFF; c->ext[215] = EMMC_SECTORS >> 24;
    c->ext[221] = 16;                                        // HC_WP_GRP_SIZE
    c->ext[224] = 1;                                         // HC_ERASE_GRP_SIZE
    c->ext[226] = 0x20;                                      // BOOT_SIZE_MULT
    emmc_save_ext(c);
    return 0;
}
// Response words as eSDHC CMDRSP0..3 (bits 127:8 for R2). Returns 0 if the command times out.
static int emmc_command(mcboard *b, int idx, uint32_t arg, int data, uint32_t rsp[4]) {
    static const uint32_t cid[4] = {0x5A2B3C21, 0x34521012, 0x34465445, 0x00150100};   // MID 0x15, PNM "4FTE4R"
    static const uint32_t csd[4] = {0x0A4040E3, 0x3FFF8000, 0x5A5F0003, 0x00D02700};
    emmc *c = &b->card;
    memset(rsp, 0, 16);
    b->n_cmd++;
    if (emmc_log()) fprintf(stderr, "[emmc] CMD%d %08x%s\n", idx, arg, data ? " +data" : "");
    switch (idx) {
    case 0: return 1;
    case 1: rsp[0] = 0xC0FF8080; return 1;                   // OCR: ready, sector addressing
    case 2: case 10: memcpy(rsp, cid, 16); return 1;
    case 3: rsp[0] = 0x500; return 1;
    case 9: memcpy(rsp, csd, 16); return 1;
    case 6: {
        int index = arg >> 16 & 0xFF, value = arg >> 8 & 0xFF;
        c->ext[index] = (uint8_t)value;
        emmc_save_ext(c);
        rsp[0] = (4 << 9) | (1 << 8);
        return 1;
    }
    case 35: case 36: case 38: b->n_erase += idx == 38; /* fallthrough */
    case 7: case 12: case 13: case 16: case 23: case 14: case 19: case 17: case 18: case 24: case 25:
        rsp[0] = (4 << 9) | (1 << 8);
        return 1;
    case 8:
        if (data) { rsp[0] = (4 << 9) | (1 << 8); return 1; }
        return 0;
    }
    return 0;
}
// Data from card to host memory order (word-swapped). n bytes.
static void emmc_read(mcboard *b, int idx, uint32_t arg, uint8_t *out, size_t n) {
    emmc *c = &b->card;
    uint8_t *tmp = calloc(1, n + 4);
    if (idx == 8) {
        memcpy(tmp, c->ext, n < 512 ? n : 512);
    } else if ((idx == 17 || idx == 18) && emmc_in_range(b, idx, arg, n)) {
        fseeko(c->f, (off_t)arg * 512, SEEK_SET);
        size_t got = fread(tmp, 1, n, c->f);                 // short only past the end of a sparse image: zeros
        if (got < n && ferror(c->f)) { emmc_error(b, "image read failed", idx, arg); clearerr(c->f); }
        b->n_rd += n / 512;
    }
    if (idx == 14) {                                         // BUS_TEST_R: inverted pattern, host order already
        for (size_t i = 0; i < n; i++) out[i] = (uint8_t)~(i < (size_t)c->bus_test_len ? c->bus_test[i] : 0x5A);
    } else {
        swap32_copy(out, tmp, n);
    }
    free(tmp);
}
static void emmc_write(mcboard *b, int idx, uint32_t arg, const uint8_t *in, size_t n) {
    emmc *c = &b->card;
    if (idx == 19) {
        c->bus_test_len = n < 8 ? (int)n : 8;
        memcpy(c->bus_test, in, c->bus_test_len);
        return;
    }
    if ((idx == 24 || idx == 25) && emmc_in_range(b, idx, arg, n)) {
        uint8_t *tmp = malloc(n);
        swap32_copy(tmp, in, n);
        fseeko(c->f, (off_t)arg * 512, SEEK_SET);
        if (fwrite(tmp, 1, n, c->f) != n) { emmc_error(b, "image write failed", idx, arg); clearerr(c->f); }
        free(tmp);
        b->n_wr += n / 512;
    }
}

// ---------------------------------------------------------------- eSDHC (0xFC0CC000) + eDMA channel 59
#define SD 0xFC0CC000u
#define TCD59 0xFC045760u
static void sd_line(mcboard *b) {                            // INTC2 source 31
    set_line(b, 2, 31, (b->sd_irq & io_get(b, SD + 0x38, 4)) != 0);
}
static void dma_lines(mcboard *b) {
    set_line(b, 1, 42, (b->dma_int >> 50) & 1);
    set_line(b, 2, 0, (b->dma_int >> 59) & 1);
}

// One 32-frame boundary of the audio DMA: the half about to play goes to the FIFO, then the half/major
// interrupt asks the OS (0x400589c0 -> forced render interrupt) to fill the half just played.
static void dma50_block(mcboard *b) {
    uint32_t src = b->dma50_base;
    if (src < 0x40000000u || src >= 0x50000000u) return;
    const uint8_t *ring = b->ddr + ((src - 0x40000000u) & (DDR_SIZE - 1));
    const uint8_t *half = ring + 256 * b->dma50_half;
    uint64_t w = atomic_load(&b->afifo_w);
    if (w - atomic_load(&b->afifo_r) <= AFIFO - 32) {
        for (int i = 0; i < 32; i++) {
            float *o = b->afifo + 2 * ((w + i) & (AFIFO - 1));
            o[0] = (float)((int32_t)be32(half + 8 * i) / -8388608.0);      // 24-bit samples, right-justified; the
            o[1] = (float)((int32_t)be32(half + 8 * i + 4) / -8388608.0);  // OS sends the codec the mix inverted
                                                                             // (USB carries it upright): undo that
        }
        atomic_store(&b->afifo_w, w + 32);
    }
    io_set(b, 0xFC045656u, 2, b->dma50_half ? 32 : 64);     // CITER (counts down): playing half 0 -> 64 left
    io_set(b, 0xFC045640u, 4, src + 256u * b->dma50_half);  // SADDR moves with playback: the render code
                                                             // (0x40058c72) writes the half the DMA is not in
    b->dma50_half ^= 1;
    b->n_blocks++;
    b->dma_int |= 1ull << 50;
    dma_lines(b);
}
static int usb_streaming(mcboard *b);
static double usb_next_uframe(mcboard *b);
static void dma50_check(mcboard *b, double t) {
    if (!b->dma50_enabled || t < b->dma50_due) return;
    if ((b->dma_int >> 50) & 1) { b->rt.wait_irq++; return; }   // the OS has not taken the previous block's
                                                             // interrupt yet (0x400589c0, which sets 0x40a78e0c and
                                                             // clears INT 50): another block now would play a half
                                                             // it never rendered (the one from two blocks before)
    if (b->ddr[0x40a78e0fu - 0x40000000u]) { b->rt.wait_render++; return; }   // previous block not rendered yet (0x40a78e0c)
    if (usb_streaming(b) && t - b->dma50_due < USB_WAIT_US &&
        (usb_next_uframe(b) < b->dma50_due || (b->usb.sts & b->usb.intr)))
        { b->rt.wait_usb++; return; }                        // USB audio: earlier microframes (and their
                                                             // interrupt) first, so the OS sees events in order;
                                                             // but never for long: the main output wins
    if (t - b->dma50_due > b->rt.late_max) b->rt.late_max = t - b->dma50_due;
    b->rt.late += t - b->dma50_due > 2000;
    if (t - b->dma50_due > 20000) { b->dma50_due = t; b->rt.resync++; }   // after a stall, resync rather than burst
    b->dma50_stamp = b->dma50_due;
    dma50_block(b);
    b->dma50_due += BLOCK_US;
}

static void dma59_finish(mcboard *b) {
    uint32_t csr = io_get(b, TCD59 + 0x1E, 2) | 0x80;        // DONE
    io_set(b, TCD59 + 0x1E, 2, csr);
    b->dma59_enabled = 0;
    if (csr & 2) {                                           // INTMAJOR -> INTC2 source 0 (vector 192)
        b->dma_int |= 1ull << 59;
        dma_lines(b);
    }
}
static void sd_command(mcboard *b, uint32_t x) {
    int idx = x >> 24 & 0x3F, data = x >> 21 & 1, rd = x >> 4 & 1;
    uint32_t blk = io_get(b, SD + 0x04, 4), arg = io_get(b, SD + 0x08, 4);
    int nblk = blk >> 16 ? (int)(blk >> 16) : 1, bsize = blk & 0x1FFF;
    if (!emmc_command(b, idx, arg, data, b->sd_rsp)) {
        emmc_error(b, "command not answered", idx, arg);     // (the OS's driver never sends one in normal use)
        b->sd_irq |= 1u << 16;                               // CTOE
        sd_line(b);
        return;
    }
    uint32_t irq = 1;                                        // CC
    size_t n = (size_t)nblk * bsize;
    if (data && rd) {
        if (b->dma59_enabled) {
            uint32_t daddr = io_get(b, TCD59 + 0x10, 4);
            uint8_t *buf = malloc(n);
            emmc_read(b, idx, arg, buf, n);
            uc_mem_write(b->uc, daddr, buf, n);
            free(buf);
            dma59_finish(b);
        } else {
            uint8_t buf[512];
            size_t m = n < sizeof buf ? n : sizeof buf;
            emmc_read(b, idx, arg, buf, m);
            b->sd_rn = (int)(m / 4);
            b->sd_rpos = 0;
            for (int i = 0; i < b->sd_rn; i++) b->sd_rbuf[i] = be32(buf + 4 * i);
        }
        irq |= 2;                                            // TC: data is in host memory / buffer
    } else if (data) {
        if (b->dma59_enabled) {
            uint32_t saddr = io_get(b, TCD59 + 0x00, 4);
            uint8_t *buf = malloc(n);
            uc_mem_read(b->uc, saddr, buf, n);
            emmc_write(b, idx, arg, buf, n);
            free(buf);
            dma59_finish(b);
            irq |= 2;
        } else {
            b->sd_wneed = (int)(n < sizeof b->sd_wbuf ? n : sizeof b->sd_wbuf);
            b->sd_wn = 0;
            b->sd_wcmd = idx;
            b->sd_warg = arg;
        }
    }
    b->sd_irq |= irq;
    sd_line(b);
}
static uint32_t sd_read(mcboard *b, uint32_t o) {
    if (o >= 0x10 && o < 0x20) return b->sd_rsp[(o - 0x10) / 4];
    switch (o) {
    case 0x20: return b->sd_rpos < b->sd_rn ? b->sd_rbuf[b->sd_rpos++] : 0;
    case 0x24: {                                             // PRSSTAT: lines high, card in, clock stable
        uint32_t v = 0xFF800000u | 1u << 16 | 1u << 18 | 1u << 3;
        if (b->sd_rpos < b->sd_rn) v |= 1u << 11;            // BREN
        if (b->sd_wneed) v |= 1u << 10;                      // BWEN
        return v;
    }
    case 0x2C: return (io_get(b, SD + 0x2C, 4) & ~0x0F000000u) | 1u << 3;   // resets self-clear
    case 0x30: return b->sd_irq;
    case 0x40: return 1u << 24 | 1u << 22 | 1u << 21;        // HOSTCAPBLT
    }
    return io_get(b, SD + o, 4);
}
static void sd_write(mcboard *b, uint32_t o, uint32_t v) {
    switch (o) {
    case 0x0C: sd_command(b, v); return;
    case 0x30: b->sd_irq &= ~v; sd_line(b); return;          // IRQSTAT w1c
    case 0x38: sd_line(b); return;                           // IRQSIGEN changed
    case 0x20:
        if (b->sd_wneed) {
            put_be32(b->sd_wbuf + b->sd_wn, v);
            b->sd_wn += 4;
            if (b->sd_wn >= b->sd_wneed) {
                emmc_write(b, b->sd_wcmd, b->sd_warg, b->sd_wbuf, b->sd_wn);
                b->sd_wneed = 0;
                b->sd_irq |= 2;
                sd_line(b);
            }
        }
        return;
    }
}

// ---------------------------------------------------------------- MMIO dispatch
static void *mmio_ctx[2][2];
static void dma_run(mcboard *b, int ch, int depth);
static void midi_tx_dma(mcboard *b);
static void usb_microframes(mcboard *b, double t, int chained);
static uint32_t usb_reg_read(mcboard *b, uint32_t o);
static void usb_reg_write(mcboard *b, uint32_t o, uint32_t v);
static void dma_irq_line(mcboard *b, int ch);
static uint64_t mmio_read(uc_engine *uc, uint64_t off, unsigned size, void *user) {
    mcboard *b = ((mcboard **)user)[0];
    uint32_t a = (uint32_t)(uintptr_t)((void **)user)[1] + (uint32_t)off;
    (void)uc;
    if (a >= 0xEC094018u && a < 0xEC094024u && size == 1) {  // GPIO pin data
        int n = a - 0xEC094018u;
        uint8_t v = b->gpio_out[n];
        if (n == 2) v = (v & ~0x08) | ((b->gpio_out[3] & 0x10) ? 0x08 : 0);   // board link (0x4008a7d4)
        if (n == 0) v = (v & ~PORTA_POWER_KEY) | (atomic_load(&b->power_key) ? PORTA_POWER_KEY : 0);
        return v;
    }
    if (a >= USB + 0x080 && a < USB + 0x200 && size == 4) return usb_reg_read(b, a - USB);
    if (a == UART9 + 0x04) return 0x0C;                      // USR: TxRDY, TxEMP; received bytes go by DMA
    if (a == UART9 + 0x0C) return b->uart_rb;                // URB (read by eDMA channel 36)
    if (a == UART9 + 0x14) return 0x01;                      // UISR: TxRDY
    if (a == 0xFC05800Cu) return 0x02;                       // I2C0 status: idle, done, ACK (audio codec)
    if ((a & 0xFFFF3FFFu) == 0xFC080000u && size == 2) {     // PIT PCSR
        uint32_t v = io_get(b, a, 2);
        return (v & 8) ? v : v | 4;                          // no interrupt: busy-wait delays end at once
    }
    if (a == 0xFC05C02Cu) {                                  // DSPI0 SR (flash)
        int n = b->dspi0_n > 15 ? 15 : b->dspi0_n;
        return 1u << 31 | 1u << 28 | 1u << 25 | (uint32_t)n << 4;
    }
    if (a == 0xFC05C038u) {                                  // DSPI0 POPR
        if (!b->dspi0_n) return 0;
        uint8_t v = b->dspi0_rx[0];
        memmove(b->dspi0_rx, b->dspi0_rx + 1, --b->dspi0_n);
        return v;
    }
    if (a == 0xFC03C02Cu) return 1u << 31 | 1u << 28 | 1u << 25;   // DSPI1 SR (screen): always done
    if (a == 0xFC094012u && size == 2) {                     // ADC RSLT0: pad pressure (0x40059d7a)
        int col = b->fp_col & 7;
        return col < 6 ? (uint32_t)atomic_load(&b->pad_level[6 - col]) : 0;
    }
    if ((a & 0xFFFF3FFFu) == 0xFC07000Cu)                    // DMA timer counter
        return dtim_count(b, (int)((a - 0xFC070000u) >> 14));
    if (a >= SD && a < SD + 0x100 && size == 4) return sd_read(b, a - SD);
    if (a == 0xFC044024u) return (uint32_t)b->dma_int;       // eDMA INTL
    if (a == 0xFC044020u) return (uint32_t)(b->dma_int >> 32);
    return io_get(b, a, size);
}
static void mmio_write(uc_engine *uc, uint64_t off, unsigned size, uint64_t value, void *user) {
    mcboard *b = ((mcboard **)user)[0];
    uint32_t a = (uint32_t)(uintptr_t)((void **)user)[1] + (uint32_t)off;
    uint32_t v = (uint32_t)value;
    (void)uc;
    io_set(b, a, size, v);
    double t = now_us() - b->t0;
    if (size == 1 && a >= 0xEC094000u && a < 0xEC09400Cu) b->gpio_out[a - 0xEC094000u] = v;
    else if (size == 1 && a >= 0xEC094018u && a < 0xEC094024u) b->gpio_out[a - 0xEC094018u] |= v;
    else if (size == 1 && a >= 0xEC094024u && a < 0xEC094030u) b->gpio_out[a - 0xEC094024u] &= v;
    else if (a >= USB + 0x080 && a < USB + 0x200 && size == 4) usb_reg_write(b, a - USB, v);
    else if (a == UART9 + 0x0C && size == 1) {               // UTB: a MIDI byte out
        uint64_t w = atomic_load(&b->mout_w);
        if (w - atomic_load(&b->mout_r) < MIDI_RING) { b->midi_out[w & (MIDI_RING - 1)] = (uint8_t)v; atomic_store(&b->mout_w, w + 1); }
        b->n_midi_out++;
    } else if (a == UART9 + 0x14 && size == 1) {             // UIMR: TxRDY interrupt -> INTC1 source 53
        b->uart_imr = (uint8_t)v;
        set_line(b, 1, 53, v & 1);
    }
    else if (a == 0xFC058010u && size == 1) {                // I2C0 I2DR: codec address, register, value
        if (v == 0x34 && b->i2c_phase != 1) b->i2c_phase = 1;
        else if (b->i2c_phase == 1) { b->i2c_reg = (int)v; b->i2c_phase = 2; }
        else if (b->i2c_phase == 2) {
            b->codec[b->i2c_reg & 0xFF] = (uint8_t)v; b->codec_writes++; b->i2c_phase = 0;
            if (getenv("MCB_CODECLOG")) fprintf(stderr, "[codec] reg %02x = %02x\n", b->i2c_reg, v);
        }
    }
    else if (a == 0xFC05C034u) {                             // DSPI0 PUSHR -> flash
        if (b->dspi0_n < (int)sizeof b->dspi0_rx) b->dspi0_rx[b->dspi0_n++] = flash_xfer(&b->flash, v & 0xFF);
        if (!(v >> 31)) { b->flash.cmd = -1; flash_save(b); }
    } else if (size == 1 && ((a & 0xFFFu) == 0x01C || (a & 0xFFFu) == 0x01D) &&
               (a >> 12 == 0xFC048 || a >> 12 == 0xFC04C || a >> 12 == 0xFC050)) {
        int n = a >> 12 == 0xFC048 ? 0 : a >> 12 == 0xFC04C ? 1 : 2;   // SIMR / CIMR
        uint64_t bit = (v & 0x40) ? ~0ull : 1ull << (v & 63);
        b->imr[n] = (a & 1) ? b->imr[n] & ~bit : b->imr[n] | bit;
        io_set(b, (a & ~0xFFFu) + 0x08, 4, (uint32_t)(b->imr[n] >> 32));
        io_set(b, (a & ~0xFFFu) + 0x0C, 4, (uint32_t)b->imr[n]);
        irq_update(b);
    } else if ((a >> 12 == 0xFC048 || a >> 12 == 0xFC04C || a >> 12 == 0xFC050) && (a & 0xFFFu) >= 0x08 &&
               (a & 0xFFFu) < 0x10) {
        int n = a >> 12 == 0xFC048 ? 0 : a >> 12 == 0xFC04C ? 1 : 2;   // IMRH (0x08) / IMRL (0x0C) written directly
        uint32_t base = a & ~0xFFFu;
        b->imr[n] = (uint64_t)io_get(b, base + 0x08, 4) << 32 | io_get(b, base + 0x0C, 4);
        irq_update(b);
    } else if ((a >> 12 == 0xFC048 || a >> 12 == 0xFC04C || a >> 12 == 0xFC050) && (a & 0xFFFu) >= 0x10 &&
               (a & 0xFFFu) < 0x18) {
        int n = a >> 12 == 0xFC048 ? 0 : a >> 12 == 0xFC04C ? 1 : 2;   // INTFRCH (0x10) / INTFRCL (0x14)
        uint32_t base = a & ~0xFFFu;
        b->force[n] = (uint64_t)io_get(b, base + 0x10, 4) << 32 | io_get(b, base + 0x14, 4);
        irq_update(b);   // the panel scan forces INTC1 source 2 for the 1 kHz key/encoder handler
    } else if (a >= 0xFC048040u && a < 0xFC054000u && (a & 0x3FFF) >= 0x40 && (a & 0x3FFF) < 0x80) {
        irq_update(b);                                       // ICR (level) changed
    } else if ((a & 0xFFFF3FFFu) == 0xFC080000u && size == 2) {
        int n = (a - 0xFC080000u) >> 14;                     // PIT PCSR: PIF w1c, program
        uint32_t pcsr = v;
        if (pcsr & 4) { io_set(b, a, 2, pcsr & ~4u); set_line(b, 2, 13 + n, 0); }
        pit_program(b, n, t);
        pit_check(b, n, t);
    } else if ((a & 0xFFFF3FFFu) == 0xFC080002u && size == 2) {
        pit_program(b, (a - 0xFC080000u) >> 14, t);
    } else if ((a & 0xFFFF3FFFu) == 0xFC070000u && size == 2) {
        dtim_program(b, (a - 0xFC070000u) >> 14, t);
    } else if ((a & 0xFFFF3FFFu) == 0xFC070003u && size == 1) {   // DTER w1c
        int n = (a - 0xFC070000u) >> 14;
        io_set(b, a, 1, 0);
        set_line(b, 0, 32 + n, 0);
    } else if (a == 0xFC044018u && size == 1) {              // eDMA SERQ
        if ((v & 0x3F) == 59 || (v & 0x40)) b->dma59_enabled = 1;
        if (((v & 0x3F) == 50 || (v & 0x40)) && !b->dma50_enabled) {
            b->dma50_enabled = 1;
            b->dma50_half = 0;
            b->dma50_base = io_get(b, 0xFC045640u, 4);
            b->dma50_due = t + BLOCK_US;
        }
        b->erq |= (v & 0x40) ? ~0ull : 1ull << (v & 63);
        if ((v & 0x40) == 0 && (v & 0x3F) == 37) midi_tx_dma(b);
    } else if (a == 0xFC044019u && size == 1) {              // eDMA CERQ
        b->erq &= (v & 0x40) ? 0 : ~(1ull << (v & 63));
        if ((v & 0x3F) == 59 || (v & 0x40)) b->dma59_enabled = 0;
        if ((v & 0x3F) == 50 || (v & 0x40)) b->dma50_enabled = 0;
    } else if (a == 0xFC04401Cu && size == 1) {              // eDMA CINT
        b->dma_int &= (v & 0x40) ? 0 : ~(1ull << (v & 63));
        dma_lines(b);
        if (!(v & 0x40)) dma_irq_line(b, v & 63);           // software-run channels (MIDI 36/37, mixer copies)
        else for (int ch = 0; ch < 64; ch++) dma_irq_line(b, ch);
    } else if (a >= SD && a < SD + 0x100 && size == 4) {
        sd_write(b, a - SD, v);
    } else if (a >= 0xFC045000u && a < 0xFC045800u && (a & 31) == 0x1E && size == 2 && (v & 1)) {
        int ch = (int)((a - 0xFC045000u) >> 5);              // TCD CSR with START: software-started transfer
        if (ch != 50 && ch != 59) dma_run(b, ch, 0);
    } else if (a == 0xFC04401Du && size == 1) {              // eDMA SSRT
        int ch = v & 63;
        if (ch != 50 && ch != 59) { io_set(b, 0xFC045000u + 32u * ch + 0x1E, 2, io_get(b, 0xFC045000u + 32u * ch + 0x1E, 2) | 1); dma_run(b, ch, 0); }
    }
}

// ---------------------------------------------------------------- front panel bus (0x8C000000)
// The 12 kHz PIT3 handler (0x40059cd0) writes the column number to 0x8C000002 bits 7:5, reads that
// column's keys back from 0x8C000002, and drives LED latches through 0x8C000000/06/0A.
// Columns 0..3: 31 keys (bit = 1 while pressed). Columns 4..7: four encoders each, bits (2j, 2j+1) a
// quadrature pair decoded by 0x40059c18/0x40059c74: forward is Gray state (a<<1|b) 0 -> 1 -> 3 -> 2 -> 0, and
// the OS counts one knob event per two transitions. We move each encoder one transition per scan.
static const uint8_t gray_next[4] = {1, 3, 0, 2}, gray_prev[4] = {2, 0, 3, 1};
static uint64_t fp_read(uc_engine *uc, uint64_t off, unsigned size, void *user) {
    mcboard *b = user;
    (void)uc; (void)size;
    if ((off & ~1ull) != 2) return 0xFF;
    b->n_fp_reads++;
    int col = b->fp_col & 7;
    if (col < 4) return b->keys[col];
    // Advance only after the 1 kHz decoder (0x40059f18) has taken the last value: it copies each column
    // byte to 0x40a7918c when done. Two steps between decoder runs would look like an invalid jump.
    int consumed = b->ddr[0x40a7918cu - 0x40000000u + (col - 4)] == b->enc_shown[col - 4];
    uint8_t v = 0;
    for (int j = 0; j < 4; j++) {
        int e = (col - 4) * 4 + j;
        int p = consumed ? atomic_load(&b->enc_pending[e]) : 0;
        if (p > 0) { b->enc_state[e] = gray_next[b->enc_state[e]]; atomic_fetch_sub(&b->enc_pending[e], 1); }
        else if (p < 0) { b->enc_state[e] = gray_prev[b->enc_state[e]]; atomic_fetch_add(&b->enc_pending[e], 1); }
        uint8_t st = b->enc_state[e];
        v |= (uint8_t)(((st >> 1) & 1) << (2 * j) | (st & 1) << (2 * j + 1));
    }
    b->enc_shown[col - 4] = v;
    return v;
}
static void fp_write(uc_engine *uc, uint64_t off, unsigned size, uint64_t value, void *user) {
    mcboard *b = user;
    (void)uc; (void)size;
    if (off == 2) b->fp_col = (int)(value >> 5) & 7;
    if (off < 16) b->fp_latch[off / 2] = (uint16_t)value;
}

// The render interrupt has just finished a block (clrl 0x40a78e0c at 0x400593dc): start the next one if due,
// so the audio keeps its real 1500 blocks/s inside a 1 ms slice without overtaking the renderer.
static void on_render_done(uc_engine *uc, uint64_t addr, uint32_t size, void *user) {
    mcboard *b = user;
    (void)uc; (void)addr; (void)size;
    double t = now_us() - b->t0;
    usb_microframes(b, t, 0);                               // USB audio packets, as evenly as the audio blocks
    dma50_check(b, t);
    usb_microframes(b, t, 0);
}

// The OS has run its power-off routine to the end (see POWER_OFF_SPIN): its last writes are done and it will
// never run again, so the run ends here as it would when the power goes.
static void on_power_off(uc_engine *uc, uint64_t addr, uint32_t size, void *user) {
    mcboard *b = user;
    (void)addr; (void)size;
    if (atomic_exchange(&b->powered_off, 1)) return;
    atomic_store(&b->stop, 1);
    uc_emu_stop(uc);
}

// The OS has nothing to do until its next interrupt (see IDLE_PC): end the slice, so the CPU thread can sleep.
static void on_idle(uc_engine *uc, uint64_t addr, uint32_t size, void *user) {
    mcboard *b = user;
    (void)addr; (void)size;
    b->idle = 1;
    uc_emu_stop(uc);
}

// MCB_RTLOG: the OS's USB interrupt (0x40004318) is taken.
static void on_sof_irq(uc_engine *uc, uint64_t addr, uint32_t size, void *user) {
    (void)uc; (void)addr; (void)size;
    ((mcboard *)user)->rt.sof_irq++;
}

// MCB_MIDILOG: each byte the OS's receive interrupt hands to its MIDI parser (0x400012cc).
static void on_midi_byte(uc_engine *uc, uint64_t addr, uint32_t size, void *user) {
    uint32_t sp; uint8_t v[4];
    (void)addr; (void)size; (void)user;
    uc_reg_read(uc, UC_M68K_REG_A7, &sp);
    uc_mem_read(uc, sp + 4, v, 4);
    fprintf(stderr, "[midi in] %02x\n", v[3]);
}

static void on_unmapped(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user) {
    mcboard *b = user;
    uint32_t pc;
    (void)type; (void)size; (void)value;
    uc_reg_read(uc, UC_M68K_REG_PC, &pc);
    snprintf(b->fault, sizeof b->fault, "unmapped access 0x%08llx at pc 0x%08x", (unsigned long long)addr, pc);
}

// ---------------------------------------------------------------- eDMA engine (software-started transfers)
// TCD n at 0xFC045000 + 32n: 0x00 SADDR, 0x04 ATTR (SMOD 15:11, SSIZE 10:8, DMOD 7:3, DSIZE 2:0), 0x06 SOFF,
// 0x08 NBYTES, 0x0C SLAST, 0x10 DADDR, 0x14 CITER (ELINK 15, LINKCH 14:9), 0x16 DOFF, 0x18 DLAST_SGA,
// 0x1C BITER, 0x1E CSR (START 0, INTMAJOR 1, INTHALF 2, DREQ 3, ESG 4, MAJORELINK 5, ACTIVE 6, DONE 7,
// MAJORLINKCH 13:8). A started channel runs to completion at once (the mixer's delay/reverb copies, channel
// 30, wait for ACTIVE to clear inside the render interrupt).
static void dma_irq_line(mcboard *b, int ch) {
    if (ch >= 56) set_line(b, 2, 0, (b->dma_int >> 56) & 0xFF ? 1 : 0);
    else if (ch >= 16) set_line(b, 1, 8 + (ch - 16), (b->dma_int >> ch) & 1);
    else set_line(b, 0, 8 + ch, (b->dma_int >> ch) & 1);
}
static uint32_t bus_read(mcboard *b, uint32_t a, int n) {
    if ((a >> 24) == 0xEC || (a >> 24) == 0xFC) return mmio_read(b->uc, a & 0xFFFFFF, (unsigned)n, mmio_ctx[(a >> 24) == 0xFC]);
    uint8_t t[4] = {0};
    uc_mem_read(b->uc, a, t, (size_t)n);
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | t[i];
    return v;
}
static void bus_write(mcboard *b, uint32_t a, int n, uint32_t v) {
    if ((a >> 24) == 0xEC || (a >> 24) == 0xFC) { mmio_write(b->uc, a & 0xFFFFFF, (unsigned)n, v, mmio_ctx[(a >> 24) == 0xFC]); return; }
    uint8_t t[4];
    for (int i = 0; i < n; i++) t[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    uc_mem_write(b->uc, a, t, (size_t)n);
}
static uint32_t modulo_add(uint32_t a, int32_t off, int mod) {
    if (!mod) return a + (uint32_t)off;
    uint32_t m = (1u << mod) - 1;
    return (a & ~m) | ((a + (uint32_t)off) & m);
}
static void dma_run(mcboard *b, int ch, int depth) {
    static const int unit[8] = {1, 2, 4, 0, 16, 32, 0, 0};
    for (int chain = 0; chain < 64; chain++) {
        uint32_t t = 0xFC045000u + 32u * ch;
        uint32_t sa = io_get(b, t, 4), da = io_get(b, t + 0x10, 4);
        uint32_t attr = io_get(b, t + 4, 2), nbytes = io_get(b, t + 8, 4);
        int32_t soff = (int16_t)io_get(b, t + 6, 2), doff = (int16_t)io_get(b, t + 0x16, 2);
        int32_t slast = (int32_t)io_get(b, t + 0xC, 4), dlast = (int32_t)io_get(b, t + 0x18, 4);
        uint32_t citer = io_get(b, t + 0x14, 2), csr = io_get(b, t + 0x1E, 2);
        int elink = (citer >> 15) & 1, count = elink ? (citer & 0x1FF) : (citer & 0x7FFF);
        int smod = attr >> 11 & 31, ssz = unit[attr >> 8 & 7], dmod = attr >> 3 & 31, dsz = unit[attr & 7];
        if (!ssz || !dsz || !count) break;
        io_set(b, t + 0x1E, 2, (csr | 0x40) & ~0x81u);          // ACTIVE, clear START/DONE
        uint8_t buf[32];
        for (int major = 0; major < count; major++) {
            for (uint32_t k = 0; k < nbytes; ) {
                int sz = ssz > dsz ? ssz : dsz;                    // move one unit of the larger size
                for (int i = 0; i < sz; i += ssz) {
                    for (int j = 0; j < ssz; j += 4) {
                        int w = ssz - j < 4 ? ssz - j : 4;
                        uint32_t v = bus_read(b, sa + j, w);
                        for (int q = 0; q < w; q++) buf[i + j + q] = (uint8_t)(v >> (8 * (w - 1 - q)));
                    }
                    sa = modulo_add(sa, soff, smod);
                }
                for (int i = 0; i < sz; i += dsz) {
                    for (int j = 0; j < dsz; j += 4) {
                        int w = dsz - j < 4 ? dsz - j : 4;
                        uint32_t v = 0;
                        for (int q = 0; q < w; q++) v = v << 8 | buf[i + j + q];
                        bus_write(b, da + j, w, v);
                    }
                    da = modulo_add(da, doff, dmod);
                }
                k += (uint32_t)sz;
            }
            if (elink && ((citer >> 9) & 63) != ch && depth < 4) dma_run(b, (citer >> 9) & 63, depth + 1);
        }
        io_set(b, t + 0x14, 2, io_get(b, t + 0x1C, 2));          // CITER = BITER
        int esg = (csr >> 4) & 1;
        io_set(b, t, 4, sa + (uint32_t)slast);
        if (esg) {                                               // scatter-gather: load the next TCD from memory
            uint8_t next[32];
            uc_mem_read(b->uc, (uint32_t)dlast, next, 32);
            for (int i = 0; i < 32; i++) io_ptr(b, t)[i] = next[i];
        } else {
            io_set(b, t + 0x10, 4, da + (uint32_t)dlast);
        }
        uint32_t ncsr = io_get(b, t + 0x1E, 2);
        if (!esg) io_set(b, t + 0x1E, 2, (ncsr & ~0x41u) | 0x80);   // DONE, not ACTIVE
        if (csr & 2) { b->dma_int |= 1ull << ch; dma_irq_line(b, ch); }
        if ((csr & 0x20) && depth < 4) dma_run(b, (csr >> 8) & 63, depth + 1);
        if (!(esg && (ncsr & 1))) {
            if (esg) io_set(b, t + 0x1E, 2, (ncsr & ~0x41u) | 0x80);
            break;                                               // the next TCD runs only if it is started
        }
    }
}


// ---------------------------------------------------------------- USB (OTG controller in device mode + a host)
// The OS's driver (0x40004318 hardware interrupt: SOF stamps, VBUS changes, then a forced INTC1 source 6 for
// the rest at 0x40005078) programs a ChipIdea-style controller: USBCMD 0x140, USBSTS 0x144 (w1c), USBINTR 0x148,
// FRINDEX 0x14C, DEVICEADDR 0x154, EPLISTADDR 0x158, PORTSC 0x184, OTGSC 0x1A4, USBMODE 0x1A8, ENDPTSETUPSTAT
// 0x1AC, ENDPTPRIME 0x1B0, ENDPTFLUSH 0x1B4, ENDPTSTAT 0x1B8, ENDPTCOMPLETE 0x1BC, ENDPTCTRL0-3 0x1C0.
// dQH n (64 bytes, n = ep*2 + IN) at EPLISTADDR: +0 caps, +8 next dTD, +0xC token, +0x28 setup bytes.
// dTD: +0 next (bit 0 = end), +4 token (bytes 30:16, IOC 15, status 7:0, 0x80 active), +8.. buffer pages.
// Our host runs between CPU slices, so a transfer completes within about a millisecond.
enum { USB_OFF, USB_RESET, USB_ENUM, USB_READY, USB_FAILED };
#define USBSTS_UI 1u
#define USBSTS_PCI 4u
#define USBSTS_URI 0x40u
#define USBSTS_SRI 0x80u
static int usb_log(void) { static int v = -1; if (v < 0) v = getenv("MCB_USBLOG") != NULL; return v; }
static uint32_t gread32(mcboard *b, uint32_t a) { uint8_t t[4]; uc_mem_read(b->uc, a, t, 4); return be32(t); }
static void gwrite32(mcboard *b, uint32_t a, uint32_t v) { uint8_t t[4]; put_be32(t, v); uc_mem_write(b->uc, a, t, 4); }
static void usb_line(mcboard *b) {                          // INTC2 source 17
    uint32_t otg = (b->usb.otgsc >> 16) & (b->usb.otgsc >> 24) & 0x7F;
    set_line(b, 2, 17, (b->usb.sts & b->usb.intr) || otg);
}
static uint32_t ep_bit(int idx) { return (idx & 1) ? 1u << (16 + idx / 2) : 1u << (idx / 2); }
static uint32_t usb_frindex(mcboard *b) {
    if (!(b->usb.cmd & 1)) return 0;
    return (uint32_t)(b->usb.uframe ? b->usb.uframe - 1 : 0) & 0x3FFF;   // the microframe on the bus: the OS
                                                                         // stamps its SOF table by it
}
// Moves up to `max` bytes between `buf` and the head dTD of endpoint idx and retires it. Returns bytes moved,
// or -1 if no dTD is waiting. For IN endpoints (device -> host) the dTD's data lands in buf.
static int usb_xfer(mcboard *b, int idx, uint8_t *buf, int max) {
    uint32_t d = b->usb.cur[idx];
    if (!d) return -1;
    uint32_t tok = gread32(b, d + 4);
    if (!(tok & 0x80)) return -1;
    int total = (int)((tok >> 16) & 0x7FFF), n = total < max ? total : max;
    uint32_t page = gread32(b, d + 8);
    for (int i = 0; i < n; ) {                                // walk the buffer pages (4 KB each)
        uint32_t a = page + (uint32_t)i;
        int pg = i ? ((int)((page & 0xFFF) + i) >> 12) : 0;
        if (pg) a = (gread32(b, d + 8 + 4 * (uint32_t)pg) & ~0xFFFu) + ((page + (uint32_t)i) & 0xFFF);
        int chunk = 0x1000 - (int)(a & 0xFFF);
        if (chunk > n - i) chunk = n - i;
        if (idx & 1) uc_mem_read(b->uc, a, buf + i, (size_t)chunk);
        else uc_mem_write(b->uc, a, buf + i, (size_t)chunk);
        i += chunk;
    }
    tok = (tok & 0x8000FF00u) | (uint32_t)(total - n) << 16;   // bytes left; status: done (not active)
    gwrite32(b, d + 4, tok);
    uint32_t q = b->usb.eplist + 64u * (uint32_t)idx, next = gread32(b, d);
    gwrite32(b, q + 4, d);
    gwrite32(b, q + 8, next);
    gwrite32(b, q + 0xC, tok);
    b->usb.cur[idx] = (next & 1) ? 0 : (next & ~31u);
    if (!b->usb.cur[idx]) b->usb.epsr &= ~ep_bit(idx);
    if (tok & 0x8000) { b->usb.complete |= ep_bit(idx); b->usb.sts |= USBSTS_UI; usb_line(b); }
    return n;
}
static void usb_prime(mcboard *b, uint32_t bits) {
    for (int idx = 0; idx < 8; idx++) {
        if (!(bits & ep_bit(idx)) || b->usb.cur[idx]) continue;
        uint32_t next = gread32(b, b->usb.eplist + 64u * (uint32_t)idx + 8);
        if (next & 1) continue;
        b->usb.cur[idx] = next & ~31u;
        b->usb.epsr |= ep_bit(idx);
    }
}
static void usb_reset_controller(mcboard *b) {
    memset(b->usb.cur, 0, sizeof b->usb.cur);
    b->usb.epsr = b->usb.complete = b->usb.setupstat = b->usb.sts = 0;
}
// GPTIMERnLD 0x80/0x88, GPTIMERnCTRL 0x84/0x8C: RUN 31, RST 30 (reload), MODE 24 (repeat), count 23:0.
static void usb_gpt_service(mcboard *b, double t) {
    for (int n = 0; n < 2; n++) {
        if (!(b->usb.gpt_ctrl[n] & 0x80000000u) || !b->usb.gpt_due[n] || t < b->usb.gpt_due[n]) continue;
        b->usb.sts |= 1u << (24 + n);
        if (b->usb.gpt_ctrl[n] & 0x01000000u) {
            b->usb.gpt_due[n] += b->usb.gpt_ld[n] + 1.0;
            if (t - b->usb.gpt_due[n] > 50000) b->usb.gpt_due[n] = t + b->usb.gpt_ld[n] + 1.0;
        } else {
            b->usb.gpt_due[n] = 0;
            b->usb.gpt_ctrl[n] &= ~0x80000000u;
        }
        usb_line(b);
    }
}
static uint32_t usb_reg_read(mcboard *b, uint32_t o) {
    switch (o) {
    case 0x080: case 0x088: return b->usb.gpt_ld[(o - 0x80) / 8];
    case 0x084: case 0x08C: {
        int n = (o - 0x84) / 8;
        double left = b->usb.gpt_due[n] ? b->usb.gpt_due[n] - (now_us() - b->t0) : 0;
        return (b->usb.gpt_ctrl[n] & 0xFF000000u) | ((uint32_t)(left > 0 ? left : 0) & 0xFFFFFF);
    }
    case 0x140: return b->usb.cmd;
    case 0x144: return b->usb.sts;
    case 0x148: return b->usb.intr;
    case 0x14C: return usb_frindex(b);
    case 0x154: return b->usb.devaddr;
    case 0x158: return b->usb.eplist;
    case 0x184: {                                            // PORTSC1: connected, enabled, high speed
        uint32_t v = b->usb.portsc & ~0x0C00030Du;
        if (b->usb.vbus && (b->usb.cmd & 1) && b->usb.state != USB_OFF) {
            v |= 1;                                          // CCS
            if (b->usb.state == USB_RESET) v |= 1u << 8;     // PR
            else v |= 4u | 1u << 9 | 2u << 26;               // PE, HSP, PSPD = high speed
        }
        return v;
    }
    case 0x1A4: return (b->usb.otgsc & 0x7F7F0000u) | (b->usb.vbus ? 0x0E00u : 0) | 0x100u;  // AVV ASV BSV, ID
    case 0x1AC: return b->usb.setupstat;
    case 0x1B0: case 0x1B4: return 0;                        // prime and flush take effect at once
    case 0x1B8: return b->usb.epsr;
    case 0x1BC: return b->usb.complete;
    }
    return io_get(b, USB + o, 4);
}
static void usb_reg_write(mcboard *b, uint32_t o, uint32_t v) {
    switch (o) {
    case 0x080: case 0x088: b->usb.gpt_ld[(o - 0x80) / 8] = v & 0xFFFFFF; return;
    case 0x084: case 0x08C: {
        int n = (o - 0x84) / 8;
        b->usb.gpt_ctrl[n] = v & ~0x40000000u;
        if (!(v & 0x80000000u)) b->usb.gpt_due[n] = 0;
        else if ((v & 0x40000000u) || !b->usb.gpt_due[n]) b->usb.gpt_due[n] = now_us() - b->t0 + b->usb.gpt_ld[n] + 1.0;
        return;
    }
    case 0x140: {
        int was = b->usb.cmd & 1;
        if (v & 2) { usb_reset_controller(b); v &= ~2u; }    // RST self-clears
        b->usb.cmd = v;
        if (!was && (v & 1)) {                               // run: the host sees the pull-up
            b->usb.t_run = now_us() - b->t0;
            b->usb.uframe = 0;
            b->usb.cap_alt = b->usb.play_alt = 0;
            if (b->usb.vbus && b->usb.connect) { b->usb.state = USB_RESET; b->usb.step = 0; b->usb.due = b->usb.t_run + 100000; }
            if (usb_log()) fprintf(stderr, "[usb] device running (vbus %d)\n", b->usb.vbus);
        } else if (was && !(v & 1)) {
            b->usb.state = USB_OFF;
        }
        break;
    }
    case 0x144: b->usb.sts &= ~v; break;     // (no microframe from here: the SOF interrupt reads FRINDEX again
                                             // after this acknowledge, 0x400043aa, to pick its stamp slot)
    case 0x148: b->usb.intr = v; break;
    case 0x154: b->usb.devaddr = v; break;
    case 0x158: b->usb.eplist = v & ~0x7FFu; break;
    case 0x184: b->usb.portsc = v; break;
    case 0x1A4:
        b->usb.otgsc = (b->usb.otgsc & 0x007F0000u & ~(v & 0x007F0000u)) | (v & 0x7F00007Fu);
        if ((v & (1u << 27)) && !b->usb.vbus && b->usb.connect) {   // B-session interrupt enabled: plug in
            b->usb.vbus = 1;
            b->usb.otgsc |= 1u << 19;                        // BSVIS
        }
        break;
    case 0x1AC: b->usb.setupstat &= ~v; break;
    case 0x1B0: usb_prime(b, v); break;
    case 0x1B4:
        for (int idx = 0; idx < 8; idx++) if (v & ep_bit(idx)) { b->usb.cur[idx] = 0; b->usb.epsr &= ~ep_bit(idx); }
        break;
    case 0x1BC: b->usb.complete &= ~v; break;
    default: io_set(b, USB + o, 4, v); return;
    }
    usb_line(b);
}

// ---- the host: enumeration script, then USB-MIDI
static const uint8_t usb_script[][8] = {
    {0x80, 6, 0, 1, 0, 0, 64, 0},                             // GET_DESCRIPTOR device (64)
    {0x00, 5, 7, 0, 0, 0, 0, 0},                              // SET_ADDRESS 7
    {0x80, 6, 0, 1, 0, 0, 18, 0},                             // GET_DESCRIPTOR device
    {0x80, 6, 0, 2, 0, 0, 9, 0},                              // GET_DESCRIPTOR configuration (header)
    {0x80, 6, 0, 2, 0, 0, 0, 0},                              // ... whole (length filled in)
    {0x00, 9, 0, 0, 0, 0, 0, 0},                              // SET_CONFIGURATION (value filled in)
};
static void usb_parse_config(mcboard *b, const uint8_t *c, int n) {
    int iface_class = 0, iface_sub = 0, iface = 0;
    b->usb.config_value = n > 5 ? c[5] : 1;
    for (int i = 0; i + 1 < n && c[i] >= 2; i += c[i]) {
        if (c[i + 1] == 4 && i + 7 < n) { iface = c[i + 2]; iface_class = c[i + 5]; iface_sub = c[i + 6]; }
        if (c[i + 1] == 5 && i + 3 < n && iface_class == 1 && iface_sub == 2 && (c[i + 3] & 3) == 1) {
            if (((c[i + 3] >> 4) & 3) == 1) b->usb.fb_ep = c[i + 2] & 15;           // explicit feedback
            else if (c[i + 2] & 0x80) { b->usb.cap_ep = c[i + 2] & 15; b->usb.cap_iface = iface; }
            else { b->usb.play_ep = c[i + 2] & 15; b->usb.play_iface = iface; }
        }
        if (c[i + 1] == 5 && i + 3 < n && iface_class == 1 && iface_sub == 3 && (c[i + 3] & 3) == 2) {
            if (c[i + 2] & 0x80) b->usb.midi_in_ep = c[i + 2] & 15;
            else b->usb.midi_out_ep = c[i + 2] & 15;
        }
    }
    if (usb_log()) { fprintf(stderr, "[usb] configuration descriptor:"); for (int i = 0; i < n; i++) fprintf(stderr, "%s%02x", i % 32 ? " " : "\n  ", c[i]); fprintf(stderr, "\n"); }
    if (usb_log()) fprintf(stderr, "[usb] config %d, %d bytes: MIDI out ep %d, in ep %d; audio capture ep %d (iface %d), "
                           "playback ep %d (iface %d), feedback ep %d\n", b->usb.config_value, n, b->usb.midi_out_ep,
                           b->usb.midi_in_ep, b->usb.cap_ep, b->usb.cap_iface, b->usb.play_ep, b->usb.play_iface, b->usb.fb_ep);
}
// One control transfer, advanced a phase at a time: 0 setup, 1 data (IN), 2 status.
static int usb_control(mcboard *b, const uint8_t setup[8]) {
    int len = setup[6] | setup[7] << 8;
    switch (b->usb.ctl_phase) {
    case 0:
        for (int i = 0; i < 8; i += 4) {                     // the dQH holds it as little-endian words; the
            uint8_t w[4] = {setup[i + 3], setup[i + 2], setup[i + 1], setup[i]};   // OS byte-reverses (0x4000519a)
            uc_mem_write(b->uc, b->usb.eplist + 0x28 + (uint32_t)i, w, 4);
        }
        b->usb.setupstat |= 1;
        b->usb.sts |= USBSTS_UI;
        usb_line(b);
        b->usb.ctl_got = 0;
        b->usb.ctl_in = (setup[0] & 0x80) && len;
        b->usb.ctl_phase = b->usb.ctl_in ? 1 : 2;
        return 0;
    case 1: {
        int n = usb_xfer(b, 1, b->usb.ctl_buf + b->usb.ctl_got, len - b->usb.ctl_got);
        if (n < 0) return 0;
        b->usb.ctl_got += n;
        if (b->usb.ctl_got < len && n == 64) return 0;      // more packets to come
        b->usb.ctl_phase = 2;
        return 0;
    }
    case 2: {
        uint8_t z[1];                                         // status: zero-length packet the other way
        if (usb_xfer(b, b->usb.ctl_in ? 0 : 1, z, 0) < 0) return 0;
        b->usb.ctl_phase = 0;
        return 1;
    }
    }
    return 0;
}
static void usb_host(mcboard *b, double t) {
    if (b->usb.state == USB_OFF || b->usb.state == USB_FAILED || !(b->usb.cmd & 1) || t < b->usb.due) return;
    if (b->usb.state == USB_RESET) {                         // bus reset: PORTSC.PR stays up until the OS has
        if (!b->usb.step) {                                   // taken the reset interrupt (0x40004fd0 checks it)
            b->usb.sts |= USBSTS_URI;
            usb_line(b);
            b->usb.step = 1;
            return;
        }
        if (b->usb.sts & USBSTS_URI) return;
        b->usb.sts |= USBSTS_PCI;                             // reset over: high-speed port change
        usb_line(b);
        b->usb.state = USB_ENUM;
        b->usb.step = b->usb.ctl_phase = b->usb.tries = 0;
        if (usb_log()) fprintf(stderr, "[usb] bus reset, high speed (USBINTR %08x)\n", b->usb.intr);
        b->usb.due = t + 20000;
        return;
    }
    if (b->usb.state == USB_ENUM) {
        uint8_t setup[8];
        memcpy(setup, usb_script[b->usb.step], 8);
        if (b->usb.step == 4) { setup[6] = (uint8_t)b->usb.ctl_len; setup[7] = (uint8_t)(b->usb.ctl_len >> 8); }
        if (b->usb.step == 5) setup[2] = b->usb.config_value;
        if (b->usb.ctl_phase == 0) b->usb.tries = 0;
        if (!usb_control(b, setup)) {
            if (++b->usb.tries > 2000) {                      // about 2 s without an answer
                b->usb.state = USB_FAILED;
                if (usb_log()) fprintf(stderr, "[usb] no answer to request %02x %02x (phase %d)\n", setup[0], setup[1], b->usb.ctl_phase);
            }
            return;
        }
        if (usb_log()) fprintf(stderr, "[usb] request %02x %02x %02x%02x done, %d bytes\n", setup[0], setup[1], setup[3], setup[2], b->usb.ctl_got);
        if (b->usb.step == 3) b->usb.ctl_len = b->usb.ctl_got >= 4 ? (uint16_t)(b->usb.ctl_buf[2] | b->usb.ctl_buf[3] << 8) : 9;
        if (b->usb.ctl_len > sizeof b->usb.ctl_buf) b->usb.ctl_len = sizeof b->usb.ctl_buf;
        if (b->usb.step == 4) usb_parse_config(b, b->usb.ctl_buf, b->usb.ctl_got);
        if (++b->usb.step == (int)(sizeof usb_script / sizeof usb_script[0])) b->usb.state = USB_READY;
        b->usb.due = t + 2000;
        return;
    }
    // USB_READY: audio streams on or off as the app asks (SET_INTERFACE), microframes, MIDI both ways
    usb_microframes(b, t, 0);
    if (b->usb.req_iface || b->usb.ctl_phase) {
        uint8_t setup[8] = {0x01, 0x0B, (uint8_t)b->usb.req_alt, 0, (uint8_t)b->usb.req_iface, 0, 0, 0};
        if (usb_control(b, setup)) {
            if (b->usb.req_iface == b->usb.cap_iface) b->usb.cap_alt = b->usb.req_alt;
            else b->usb.play_alt = b->usb.req_alt;
            if (usb_log()) fprintf(stderr, "[usb] interface %d alt %d\n", b->usb.req_iface, b->usb.req_alt);
            b->usb.req_iface = 0;
        }
    } else if (b->usb.restart) {                             // alt 0 for each running stream, then (below) alt 1
        if (b->usb.cap_alt) { b->usb.req_iface = b->usb.cap_iface; b->usb.req_alt = 0; }
        else if (b->usb.play_alt) { b->usb.req_iface = b->usb.play_iface; b->usb.req_alt = 0; }
        else b->usb.restart = 0;
        if (usb_log() && b->usb.req_iface) fprintf(stderr, "[usb] bus stalled: restarting interface %d\n", b->usb.req_iface);
    } else if (b->usb.cap_iface && atomic_load(&b->usb.want_cap) != b->usb.cap_alt) {
        b->usb.req_iface = b->usb.cap_iface; b->usb.req_alt = atomic_load(&b->usb.want_cap);
        atomic_store(&b->usb.cap_r, atomic_load(&b->usb.cap_w));
    } else if (b->usb.play_iface && atomic_load(&b->usb.want_play) != b->usb.play_alt) {
        b->usb.req_iface = b->usb.play_iface; b->usb.req_alt = atomic_load(&b->usb.want_play);
        b->usb.fb = 6.0; b->usb.play_acc = 0;
    }
    static const uint8_t cin_len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    if (b->usb.midi_in_ep) {
        uint8_t buf[512];
        int n = usb_xfer(b, b->usb.midi_in_ep * 2 + 1, buf, sizeof buf);
        if (n > 0 && usb_log()) { fprintf(stderr, "[usb] midi in packets:"); for (int i = 0; i < n; i++) fprintf(stderr, " %02x", buf[i]); fprintf(stderr, "\n"); }
        for (int i = 0; i + 3 < n; i += 4) {
            int k = cin_len[buf[i] & 15];
            uint64_t w = atomic_load(&b->usb.out_w);
            for (int j = 0; j < k && w - atomic_load(&b->usb.out_r) < MIDI_RING; j++, w++) b->usb.out[w & (MIDI_RING - 1)] = buf[i + 1 + j];
            atomic_store(&b->usb.out_w, w);
            b->usb.n_out += (uint64_t)k;
        }
    }
    if (b->usb.midi_out_ep) {
        uint64_t r = atomic_load(&b->usb.ev_r), w = atomic_load(&b->usb.ev_w);
        if (r != w && b->usb.cur[b->usb.midi_out_ep * 2]) {
            uint8_t buf[512];
            int n = 0;
            for (; r != w && n < (int)sizeof buf; r++, n += 4) memcpy(buf + n, b->usb.ev + 4 * (r & 4095), 4);
            int sent = usb_xfer(b, b->usb.midi_out_ep * 2, buf, n);
            if (sent > 0) { atomic_store(&b->usb.ev_r, atomic_load(&b->usb.ev_r) + (uint64_t)sent / 4); b->usb.n_in += (uint64_t)sent / 4; }
        }
    }
}
// One 125 us microframe on the bus: the capture packet comes in, the feedback value is read, a playback packet
// sized by it goes out, then SOF. With SOF interrupts on (the OS stamps each one at 0x40004318 to pace the
// streams), microframes advance one per interrupt: the next runs when the OS takes SOF, if it is due by then
// (as the PIT does), so a slice's microframes keep their order and count.
static void usb_microframe(mcboard *b) {
    uint8_t buf[1024];
    uint32_t frame = (uint32_t)(b->usb.uframe >> 3) & 0x7FF;
    b->usb.n_uframes++;
    if ((b->usb.uframe & 7) == 0) {                          // a host starts isochronous streams on a frame
        b->usb.cap_on = b->usb.cap_alt;                       // boundary: the OS aligns each stream on the SOF
        b->usb.play_on = b->usb.play_alt;                     // stamp of microframe 0 of the frame its first
    }                                                        // packet left in (0x4000264a)
    if (!b->usb.cap_alt) b->usb.cap_on = 0;
    if (!b->usb.play_alt) b->usb.play_on = 0;
    if (b->usb.cap_on && b->usb.cap_ep) {
        int idx = b->usb.cap_ep * 2 + 1;
        uint32_t d = b->usb.cur[idx];
        int n = usb_xfer(b, idx, buf, sizeof buf);
        if (n < 0) b->usb.n_cap_empty++;
        if (n >= 0) {
            gwrite32(b, d + 0xC, (gread32(b, d + 0xC) & ~0x7FFu) | frame);   // iso: frame number (0x40005244)
            uint64_t w = atomic_load(&b->usb.cap_w);
            for (int i = 0; i + 7 < n; i += 8) {
                if (w - atomic_load(&b->usb.cap_r) >= AFIFO) break;
                float *o = b->usb.cap + 2 * (w & (AFIFO - 1));
                for (int c = 0; c < 2; c++) {
                    const uint8_t *p = buf + i + 4 * c;
                    o[c] = (float)((int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.0);
                }
                w++;
            }
            atomic_store(&b->usb.cap_w, w);
            b->usb.n_cap += (uint64_t)n / 8;
        }
    }
    if (b->usb.play_on && b->usb.fb_ep) {
        int idx = b->usb.fb_ep * 2 + 1;
        uint32_t d = b->usb.cur[idx];
        if (usb_xfer(b, idx, buf, 4) == 4) {
            gwrite32(b, d + 0xC, (gread32(b, d + 0xC) & ~0x7FFu) | frame);
            double f = (buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24) / 65536.0;
            if (f > 4.0 && f < 8.0) b->usb.fb = f;            // 16.16 frames per microframe
        }
    }
    if (b->usb.play_on && b->usb.play_ep && b->usb.cur[b->usb.play_ep * 2]) {
        int idx = b->usb.play_ep * 2;
        uint32_t d = b->usb.cur[idx];
        b->usb.play_acc += b->usb.fb > 0 ? b->usb.fb : 6.0;
        int frames = (int)b->usb.play_acc;
        int cap = (int)((gread32(b, d + 4) >> 16) & 0x7FFF) / 8;
        if (frames > cap) frames = cap;
        b->usb.play_acc -= frames;
        uint64_t r = atomic_load(&b->usb.play_r), w = atomic_load(&b->usb.play_w);
        for (int i = 0; i < frames; i++) {
            float v[2] = {0, 0};
            if (r != w) { const float *s = b->usb.play + 2 * (r & (AFIFO - 1)); v[0] = s[0]; v[1] = s[1]; r++; }
            else b->usb.n_play_short++;
            for (int c = 0; c < 2; c++) {
                float x = v[c] > 1 ? 1 : v[c] < -1 ? -1 : v[c];
                uint32_t q = (uint32_t)(int32_t)lrintf(x * 2147483392.0f);
                uint8_t *o = buf + 8 * i + 4 * c;
                o[0] = (uint8_t)q; o[1] = (uint8_t)(q >> 8); o[2] = (uint8_t)(q >> 16); o[3] = (uint8_t)(q >> 24);
            }
        }
        atomic_store(&b->usb.play_r, r);
        if (usb_xfer(b, idx, buf, frames * 8) >= 0) {
            gwrite32(b, d + 0xC, (gread32(b, d + 0xC) & ~0x7FFu) | frame);
            b->usb.n_play += (uint64_t)frames;
        }
    }
    b->usb.uframe++;
    b->usb.sts |= USBSTS_SRI;                                 // SOF status is set whether or not it interrupts:
    usb_line(b);                                             // the OS stamps it at any USB interrupt
}
// A microframe whose transfers interrupt the OS (IOC, or SOF when enabled) waits for that interrupt to be
// taken before the next one runs. Runs between slices (125 us while streaming) and after each audio block.
static int usb_streaming(mcboard *b) {
    return b->usb.state == USB_READY && (b->usb.cmd & 1) && (b->usb.cap_alt || b->usb.play_alt);
}
static double usb_next_uframe(mcboard *b) { return b->usb.t_run + 125.0 * (double)b->usb.uframe; }
static int rt_on(void);
static void usb_microframes_run(mcboard *b, double t, int chained);
static void usb_microframes(mcboard *b, double t, int chained) {
    if (b->usb.state != USB_READY || !(b->usb.cmd & 1)) return;
    if (!rt_on()) { usb_microframes_run(b, t, chained); return; }
    double t1 = now_us();
    usb_microframes_run(b, t, chained);
    b->rt.usb_us += now_us() - t1;
}
static void usb_microframes_run(mcboard *b, double t, int chained) {
    double due = b->usb.t_run + 125.0 * (double)b->usb.uframe;
    if (t - due > 8000) {                                    // stalled: skip ahead rather than burst; the OS
        b->usb.n_skips++;                                    // has lost the streams' pace, so restart them
        if (usb_streaming(b)) b->usb.restart = 1;
        b->usb.uframe = (uint64_t)((t - b->usb.t_run) / 125.0);
        due = b->usb.t_run + 125.0 * (double)b->usb.uframe;
    }
    for (int k = 0; k < 64 && due <= t; k++, due += 125.0) {
        if (!chained && (b->usb.sts & b->usb.intr)) return;
        if (usb_streaming(b) && b->dma50_enabled && (b->dma50_due <= due || ((b->dma_int >> 50) & 1)))
            return;                                          // the audio block due first, and its interrupt
        chained = 0;
        usb_microframe(b);
    }
}
// Host side: raw MIDI bytes -> USB-MIDI event packets on cable 0 (one writer).
static void usb_event(mcboard *b, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint64_t w = atomic_load(&b->usb.ev_w);
    if (w - atomic_load(&b->usb.ev_r) >= 4096) return;
    uint8_t *e = b->usb.ev + 4 * (w & 4095);
    e[0] = cin; e[1] = b1; e[2] = b2; e[3] = b3;
    atomic_store(&b->usb.ev_w, w + 1);
}

// ---------------------------------------------------------------- MIDI (UART9)
// The send DMA runs its whole message at once; DREQ then drops the request, as the hardware does at the
// end of the major loop.
static void midi_tx_dma(mcboard *b) {
    dma_run(b, 37, 0);
    if (io_get(b, 0xFC045000u + 32u * 37 + 0x1E, 2) & 8) b->erq &= ~(1ull << 37);
}
// Bytes from the host arrive at the real MIDI rate. Before the OS has armed its receive DMA they are lost,
// as they would be on the hardware.
static void midi_rx_service(mcboard *b, double t) {
    if (b->midi_rx_due < t - 1000) b->midi_rx_due = t - 1000;
    while (b->midi_rx_due <= t) {
        uint64_t r = atomic_load(&b->min_r);
        if (r == atomic_load(&b->min_w)) { b->midi_rx_due = t; return; }
        b->uart_rb = b->midi_in[r & (MIDI_RING - 1)];
        atomic_store(&b->min_r, r + 1);
        if ((b->erq >> 36) & 1) { dma_run(b, 36, 0); b->n_midi_in++; }
        b->midi_rx_due += MIDI_BYTE_US;
    }
}

// ---------------------------------------------------------------- lifecycle

mcboard *mcb_create(const uint8_t *os, size_t len, const char *flash_path, const char *emmc_path, char *err,
                    size_t errlen) {
    mcboard *b = calloc(1, sizeof *b);
    pthread_mutex_init(&b->lock, NULL);
    b->ddr = calloc(1, DDR_SIZE);
    b->sram = calloc(1, 0x10000);
    b->scratch = calloc(1, 0x10000);
    b->io_ec = calloc(1, 0x1000000);
    b->io_fc = calloc(1, 0x1000000);
    b->imr[0] = b->imr[1] = b->imr[2] = ~0ull;
    for (int n = 0; n < 3; n++) { io_set(b, 0xFC048008u + 0x4000u * n, 4, ~0u); io_set(b, 0xFC04800Cu + 0x4000u * n, 4, ~0u); }
    b->afifo = calloc(2 * AFIFO, sizeof(float));
    if (len + (OS_BASE - 0x40000000u) > DDR_SIZE) { snprintf(err, errlen, "MAIN OS too large"); goto fail; }
    memcpy(b->ddr + (OS_BASE - 0x40000000u), os, len);

    // SPI flash image
    FILE *f = fopen(flash_path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open flash image %s", flash_path); goto fail; }
    fseek(f, 0, SEEK_END);
    b->flash.size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b->flash.mem = malloc(b->flash.size);
    if (fread(b->flash.mem, 1, b->flash.size, f) != b->flash.size) { fclose(f); snprintf(err, errlen, "short flash read"); goto fail; }
    fclose(f);
    b->flash.path = strdup(flash_path);
    b->flash.cmd = -1;
    if (emmc_open(&b->card, emmc_path, err, errlen)) goto fail;

    uc_err r = uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &b->uc);
    if (r) { snprintf(err, errlen, "uc_open: %s", uc_strerror(r)); goto fail; }
    uc_ctl_set_cpu_model(b->uc, UC_CPU_M68K_ANY);
    uc_mem_map_ptr(b->uc, 0x40000000, DDR_SIZE, UC_PROT_ALL, b->ddr);    // DDR2 128 MB
    uc_mem_map_ptr(b->uc, 0x48000000, DDR_SIZE, UC_PROT_ALL, b->ddr);    // its uncached mirror
    uc_mem_map_ptr(b->uc, 0x80000000, 0x10000, UC_PROT_ALL, b->sram);    // internal SRAM
    uc_mem_map_ptr(b->uc, 0x00000000, 0x10000, UC_PROT_ALL, b->scratch); // cold-boot voice init writes here
    mmio_ctx[0][0] = b; mmio_ctx[0][1] = (void *)(uintptr_t)0xEC000000u;
    mmio_ctx[1][0] = b; mmio_ctx[1][1] = (void *)(uintptr_t)0xFC000000u;
    uc_mmio_map(b->uc, 0xEC000000, 0x1000000, mmio_read, mmio_ctx[0], mmio_write, mmio_ctx[0]);
    uc_mmio_map(b->uc, 0xFC000000, 0x1000000, mmio_read, mmio_ctx[1], mmio_write, mmio_ctx[1]);
    uc_mmio_map(b->uc, 0x8C000000, 0x10000, fp_read, b, fp_write, b);   // front panel latches (FlexBus)
    uc_hook h;
    uc_hook_add(b->uc, &h, UC_HOOK_CODE, (void *)on_render_done, b, 0x400593e2, 0x400593e2);
    if (getenv("MCB_RTLOG")) uc_hook_add(b->uc, &h, UC_HOOK_CODE, (void *)on_sof_irq, b, 0x40004318, 0x40004318);
    uc_hook_add(b->uc, &h, UC_HOOK_CODE, (void *)on_power_off, b, POWER_OFF_SPIN, POWER_OFF_SPIN);
    if (!getenv("MCB_NOIDLE")) uc_hook_add(b->uc, &h, UC_HOOK_CODE, (void *)on_idle, b, IDLE_PC, IDLE_PC);
    if (getenv("MCB_MIDILOG")) uc_hook_add(b->uc, &h, UC_HOOK_CODE, (void *)on_midi_byte, b, 0x400012cc, 0x400012cc);
    uc_hook_add(b->uc, &h, UC_HOOK_MEM_UNMAPPED, (void *)on_unmapped, b, 1, 0);

    uint32_t sr = 0x2700, sp = 0x47FFF000, pc = OS_ENTRY;   // as the bootstrap leaves it: [ret][flags=0]
    uc_reg_write(b->uc, UC_M68K_REG_SR, &sr);
    uc_reg_write(b->uc, UC_M68K_REG_A7, &sp);
    uc_reg_write(b->uc, UC_M68K_REG_PC, &pc);
    b->usb.connect = getenv("MCB_NOUSB") == NULL;          // the cable is in unless told otherwise
    b->usb.cap = calloc(2 * AFIFO, sizeof(float));
    b->usb.play = calloc(2 * AFIFO, sizeof(float));
    b->t0 = now_us();
    return b;
fail:
    mcb_destroy(b);
    return NULL;
}

void mcb_destroy(mcboard *b) {
    if (!b) return;
    if (b->uc) uc_close(b->uc);
    if (b->flash.mem) flash_save(b);
    if (b->card.f) { fflush(b->card.f); fsync(fileno(b->card.f)); fclose(b->card.f); }
    free(b->flash.mem);
    free((void *)b->flash.path);
    free(b->afifo);
    free(b->usb.cap); free(b->usb.play);
    free(b->ddr); free(b->sram); free(b->scratch); free(b->io_ec); free(b->io_fc);
    pthread_mutex_destroy(&b->lock);
    free(b);
}

static void snapshot_screen(mcboard *b) {
    uint32_t p = be32(b->ddr + (SCREEN_PTR - 0x40000000u));
    if (p < 0x40000000u || p >= 0x40000000u + DDR_SIZE - 1024) return;
    const uint8_t *src = b->ddr + (p - 0x40000000u);
    pthread_mutex_lock(&b->lock);
    if (memcmp(b->screen, src, 1024)) {
        memcpy(b->screen, src, 1024);
        b->screen_gen++;
    }
    pthread_mutex_unlock(&b->lock);
}

// Slices end on a watchdog kick (every SLICE_US, or SLICE_USB_US while USB audio streams), always at a
// translation-block boundary.
static void *watchdog_main(void *arg) {
    mcboard *b = arg;
    while (!atomic_load(&b->stop)) {
        usleep(b->usb.cap_alt || b->usb.play_alt ? SLICE_USB_US : SLICE_US);
        if (atomic_load(&b->running)) uc_m68k_kick(b->uc);
    }
    return NULL;
}

// MCB_RTLOG=1: once a second, how the board kept real time: CPU slices, audio blocks (late = started more than
// 2 ms after due, resync = more than 20 ms, blocks lost), what held blocks back, USB microframes and stalls,
// SOF interrupts the OS took (0x40004318), timer 2 reads.
static int rt_on(void) { static int v = -1; if (v < 0) v = getenv("MCB_RTLOG") != NULL; return v; }
static void rt_log(mcboard *b, double t) {
    if (rt_on() && b->rt.next)
        fprintf(stderr, "[rt] %.0f s: %llu slices, %llu blocks (%llu late, latest %.1f ms, %llu resyncs; waited irq %llu, render %llu, "
                "usb %llu), %llu uframes (%llu stalls), %llu SOF irqs, %llu timer-2 reads; microframes took %.2f ms; slept %.0f ms\n", t / 1e6,
                (unsigned long long)b->rt.slices, (unsigned long long)(b->n_blocks - b->rt.blocks0),
                (unsigned long long)b->rt.late, b->rt.late_max / 1000, (unsigned long long)b->rt.resync,
                (unsigned long long)b->rt.wait_irq, (unsigned long long)b->rt.wait_render, (unsigned long long)b->rt.wait_usb,
                (unsigned long long)(b->usb.n_uframes - b->rt.uframes0), (unsigned long long)(b->usb.n_skips - b->rt.skips0),
                (unsigned long long)b->rt.sof_irq, (unsigned long long)b->rt.dtim2, b->rt.usb_us / 1000, b->rt.idle_us / 1000);
    double next = t + 1e6;
    uint64_t blocks = b->n_blocks, uframes = b->usb.n_uframes, skips = b->usb.n_skips;
    memset(&b->rt, 0, sizeof b->rt);
    b->rt.next = next; b->rt.blocks0 = blocks; b->rt.uframes0 = uframes; b->rt.skips0 = skips;
}
// Host time (us since t0) of the next event that raises an interrupt, at most `cap` after t.
static double next_event(mcboard *b, double t, double cap) {
    double d = t + cap;
#define EARLIER(x) do { double x_ = (x); if (x_ && x_ < d) d = x_; } while (0)
    for (int n = 0; n < 4; n++) { EARLIER(b->pit_due[n]); EARLIER(b->dtim_due[n]); }
    for (int n = 0; n < 2; n++) if (b->usb.gpt_ctrl[n] & 0x80000000u) EARLIER(b->usb.gpt_due[n]);
    if (b->dma50_enabled) EARLIER(b->dma50_due);
    if (b->usb.cmd & 1) EARLIER(b->usb.state == USB_READY ? usb_next_uframe(b) : b->usb.due);
    if (atomic_load(&b->min_r) != atomic_load(&b->min_w)) EARLIER(b->midi_rx_due);
#undef EARLIER
    return d;
}
// The CPU is idle with no interrupt it would take: sleep until the next event (see IDLE_PC).
static void idle_sleep(mcboard *b, uint32_t sr) {
    if (b->irq_level > (int)((sr >> 8) & 7) || atomic_load(&b->stop)) return;
    double most = b->usb.cap_alt || b->usb.play_alt ? SLICE_USB_US : IDLE_MAX_US;
    double t = now_us() - b->t0, d = next_event(b, t, most);
    if (d < t + IDLE_MIN_US) d = t + (IDLE_MIN_US < most ? IDLE_MIN_US : most);
    double us = d - (now_us() - b->t0);
    if (us < 20) return;
    struct timespec ts = { 0, (long)(us * 1000) };
    nanosleep(&ts, NULL);
    b->rt.idle_us += us;
}

int mcb_run(mcboard *b, double seconds) {
    double end = seconds > 0 ? now_us() + seconds * 1e6 : 0;
    if (atomic_load(&b->powered_off)) return 0;              // powered off: nothing runs any more
    atomic_store(&b->stop, 0);
    pthread_create(&b->watchdog, NULL, watchdog_main, b);
    uint32_t pc;
    uc_reg_read(b->uc, UC_M68K_REG_PC, &pc);
    int ret = 0;
    while (!atomic_load(&b->stop) && (!end || now_us() < end)) {
        atomic_store(&b->running, 1);
        uc_err r = uc_emu_start(b->uc, pc, 0xFFFFFFFFull, 0, 0);
        atomic_store(&b->running, 0);
        b->rt.slices++;
        uint32_t sr;
        uc_reg_read(b->uc, UC_M68K_REG_PC, &pc);
        uc_reg_read(b->uc, UC_M68K_REG_SR, &sr);             // (safe only with patch_unicorn.py's SR read fix:
        atomic_store(&b->last_pc, pc);                       // stock Unicorn's read corrupts the condition codes)
        atomic_store(&b->last_sr, sr);
        if (r != UC_ERR_OK || b->fault[0]) {
            if (!b->fault[0]) snprintf(b->fault, sizeof b->fault, "%s at pc 0x%08x", uc_strerror(r), pc);
            fprintf(stderr, "mcboard: CPU stopped: %s\n", b->fault);
            ret = 1;
            break;
        }
        if (b->idle) { b->idle = 0; idle_sleep(b, sr); }
        double t = now_us() - b->t0;
        timers_service(b, t);
        dma50_check(b, t);
        midi_rx_service(b, t);
        usb_gpt_service(b, t);
        usb_host(b, t);
        dma50_check(b, t);                                   // (USB audio interleaves the two in time order)
        if (t >= b->rt.next) rt_log(b, t);
        if (t >= b->next_screen) {
            snapshot_screen(b);
            b->next_screen = t + 16000;
        }
    }
    atomic_store(&b->stop, 1);
    pthread_join(b->watchdog, NULL);
    flash_save(b);
    fflush(b->card.f);
    return ret;
}

void mcb_stop(mcboard *b) {
    atomic_store(&b->stop, 1);
    uc_m68k_kick(b->uc);
}

void mcb_power_key(mcboard *b, int down) { atomic_store(&b->power_key, down ? 1 : 0); }
int mcb_powered_off(mcboard *b) { return atomic_load(&b->powered_off); }
uint64_t mcb_emmc_errors(mcboard *b) { return b->n_emmc_err; }

// raw encoder index for each OS encoder number (inverse of the table at 0x4010ae1c)
static const int enc_raw[17] = {-1, 3, 12, 0, 1, 2, 13, 4, 5, 6, 14, 8, 9, 10, 7, 11, 15};

void mcb_key(mcboard *b, int index, int down) {
    if (index < 0 || index > 31) return;
    uint8_t bit = (uint8_t)(1u << (index & 7));
    if (down) __atomic_fetch_or(&b->keys[index >> 3], bit, __ATOMIC_SEQ_CST);
    else __atomic_fetch_and(&b->keys[index >> 3], (uint8_t)~bit, __ATOMIC_SEQ_CST);
}

void mcb_encoder(mcboard *b, int number, int steps) {
    if (number < 1 || number > 16) return;
    atomic_fetch_add(&b->enc_pending[enc_raw[number]], 4 * steps);     // one detent = a full Gray cycle
}

void mcb_pad(mcboard *b, int pad, float pressure) {
    if (pad < 1 || pad > 6) return;
    if (pressure < 0) pressure = 0;
    if (pressure > 1) pressure = 1;
    atomic_store(&b->pad_level[pad], (int)(pressure * 0x7FF8));   // 12-bit result, left-justified
}

size_t mcb_audio(mcboard *b, float *out, size_t frames) {
    uint64_t r = atomic_load(&b->afifo_r), w = atomic_load(&b->afifo_w);
    size_t n = (size_t)(w - r) < frames ? (size_t)(w - r) : frames;
    for (size_t i = 0; i < n; i++) {
        const float *s = b->afifo + 2 * ((r + i) & (AFIFO - 1));
        out[2 * i] = s[0];
        out[2 * i + 1] = s[1];
    }
    atomic_store(&b->afifo_r, r + n);
    return n;
}

size_t mcb_audio_available(mcboard *b) {
    return (size_t)(atomic_load(&b->afifo_w) - atomic_load(&b->afifo_r));
}

void mcb_midi_in(mcboard *b, const uint8_t *data, size_t n) {
    uint64_t w = atomic_load(&b->min_w);
    for (size_t i = 0; i < n && w - atomic_load(&b->min_r) < MIDI_RING; i++, w++) b->midi_in[w & (MIDI_RING - 1)] = data[i];
    atomic_store(&b->min_w, w);
}

size_t mcb_midi_out(mcboard *b, uint8_t *out, size_t max) {
    uint64_t r = atomic_load(&b->mout_r), w = atomic_load(&b->mout_w);
    size_t n = 0;
    for (; n < max && r != w; n++, r++) out[n] = b->midi_out[r & (MIDI_RING - 1)];
    atomic_store(&b->mout_r, r);
    return n;
}

void mcb_usb_midi_in(mcboard *b, const uint8_t *data, size_t n) {
    static const uint8_t len[8] = {2, 2, 2, 2, 1, 1, 2, 0};  // data bytes for 8x..Ex
    for (size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        if (c >= 0xF8) { usb_event(b, 0xF, c, 0, 0); continue; }
        if (c == 0xF0) { b->usb.in_sysex = 1; b->usb.sx[0] = c; b->usb.sx_n = 1; b->usb.ev_status = 0; continue; }
        if (b->usb.in_sysex) {
            if (c == 0xF7 || c >= 0x80) {                     // end (a status byte also ends it)
                if (c == 0xF7) b->usb.sx[b->usb.sx_n++] = c;
                usb_event(b, (uint8_t)(4 + b->usb.sx_n), b->usb.sx[0], b->usb.sx_n > 1 ? b->usb.sx[1] : 0, b->usb.sx_n > 2 ? b->usb.sx[2] : 0);
                b->usb.in_sysex = 0;
                if (c == 0xF7) continue;
            } else {
                b->usb.sx[b->usb.sx_n++] = c;
                if (b->usb.sx_n == 3) { usb_event(b, 4, b->usb.sx[0], b->usb.sx[1], b->usb.sx[2]); b->usb.sx_n = 0; }
                continue;
            }
        }
        if (c >= 0x80) {
            b->usb.ev_status = c; b->usb.ev_n = 0;
            if (c == 0xF6) { usb_event(b, 5, c, 0, 0); b->usb.ev_status = 0; }
            continue;
        }
        uint8_t st = b->usb.ev_status;
        if (!st) continue;
        b->usb.ev_data[b->usb.ev_n++] = c;
        int need = st < 0xF0 ? len[(st >> 4) - 8] : (st == 0xF2 ? 2 : 1);
        if (b->usb.ev_n < need) continue;
        uint8_t cin = st < 0xF0 ? (uint8_t)(st >> 4) : (need == 2 ? 3 : 2);
        usb_event(b, cin, st, b->usb.ev_data[0], need > 1 ? b->usb.ev_data[1] : 0);
        b->usb.ev_n = 0;
        if (st >= 0xF0) b->usb.ev_status = 0;               // system common: no running status
    }
}

size_t mcb_usb_midi_out(mcboard *b, uint8_t *out, size_t max) {
    uint64_t r = atomic_load(&b->usb.out_r), w = atomic_load(&b->usb.out_w);
    size_t n = 0;
    for (; n < max && r != w; n++, r++) out[n] = b->usb.out[r & (MIDI_RING - 1)];
    atomic_store(&b->usb.out_r, r);
    return n;
}

void mcb_usb_audio_enable(mcboard *b, int capture, int playback) {
    atomic_store(&b->usb.want_cap, capture ? 1 : 0);
    atomic_store(&b->usb.want_play, playback ? 1 : 0);
}

size_t mcb_usb_audio_read(mcboard *b, float *out, size_t frames) {
    uint64_t r = atomic_load(&b->usb.cap_r), w = atomic_load(&b->usb.cap_w);
    size_t n = (size_t)(w - r) < frames ? (size_t)(w - r) : frames;
    for (size_t i = 0; i < n; i++) {
        const float *s = b->usb.cap + 2 * ((r + i) & (AFIFO - 1));
        out[2 * i] = s[0];
        out[2 * i + 1] = s[1];
    }
    atomic_store(&b->usb.cap_r, r + n);
    return n;
}

size_t mcb_usb_audio_read_available(mcboard *b) {
    return (size_t)(atomic_load(&b->usb.cap_w) - atomic_load(&b->usb.cap_r));
}

size_t mcb_usb_audio_write(mcboard *b, const float *in, size_t frames) {
    uint64_t r = atomic_load(&b->usb.play_r), w = atomic_load(&b->usb.play_w);
    size_t space = AFIFO - (size_t)(w - r), n = frames < space ? frames : space;
    for (size_t i = 0; i < n; i++) {
        float *o = b->usb.play + 2 * ((w + i) & (AFIFO - 1));
        o[0] = in[2 * i];
        o[1] = in[2 * i + 1];
    }
    atomic_store(&b->usb.play_w, w + n);
    return n;
}

size_t mcb_usb_audio_write_queued(mcboard *b) {
    return (size_t)(atomic_load(&b->usb.play_w) - atomic_load(&b->usb.play_r));
}

int mcb_usb_state(mcboard *b) {
    return b->usb.state == USB_READY ? 2 : b->usb.state == USB_FAILED ? -1 : (b->usb.cmd & 1) && b->usb.state != USB_OFF ? 1 : 0;
}

uint64_t mcb_leds(mcboard *b) {
    const uint8_t *p = b->ddr + (0x40140aa8u - 0x40000000u);       // latch copy, 1 = off (0x40059fd2)
    uint64_t v = 0;
    for (int i = 0; i < 7; i++) v |= (uint64_t)(uint8_t)~p[i] << (8 * i);
    return v;
}

uint64_t mcb_screen(mcboard *b, uint8_t out[1024]) {
    pthread_mutex_lock(&b->lock);
    memcpy(out, b->screen, 1024);
    uint64_t g = b->screen_gen;
    pthread_mutex_unlock(&b->lock);
    return g;
}

void mcb_status(mcboard *b, char *out, size_t len) {
    uint32_t pc = atomic_load(&b->last_pc), sr = atomic_load(&b->last_sr);
    snprintf(out, len, "t=%.1fs pc=%08x sr=%04x audio blocks=%llu midi in/out=%llu/%llu usb=%d (midi %llu/%llu, audio out/in %llu/%llu, short %llu, fb %.4f) panel reads=%llu irq=L%d/v%d emmc: %llu cmds, %llu read, %llu written, %llu erases, %llu errors%s%s%s",
             (now_us() - b->t0) / 1e6, pc, sr & 0xFFFF, (unsigned long long)b->n_blocks, (unsigned long long)b->n_midi_in, (unsigned long long)b->n_midi_out, mcb_usb_state(b), (unsigned long long)b->usb.n_in, (unsigned long long)b->usb.n_out,
             (unsigned long long)b->usb.n_cap, (unsigned long long)b->usb.n_play, (unsigned long long)b->usb.n_play_short, b->usb.fb, (unsigned long long)b->n_fp_reads, b->irq_level, b->irq_vector,
             (unsigned long long)b->n_cmd, (unsigned long long)b->n_rd, (unsigned long long)b->n_wr,
             (unsigned long long)b->n_erase, (unsigned long long)b->n_emmc_err, atomic_load(&b->powered_off) ? " POWERED OFF" : "",
             b->fault[0] ? " FAULT: " : "", b->fault);
}
