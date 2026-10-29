// Headless check of the timing the OS sees, run past the 32 s at which DMA timer 0's 32-bit counter first wraps:
// (1) DMA timer 0 keeps counting at 135.168 MHz; (2) knob events (OS knob events, as the MiniLab mapping in
// app/MiniLab.swift sends them; the OS stamps them with timer 0): a lone event after a pause changes nothing, a click
// (two events 40 ms apart) is about one step, steady events about one step each, and both ways alike (the ranges
// are what OS 1.13 does; the OS applies steps on its own tick and speeds up on fast turns); (3) tap tempo
// (FUNC + TEMPO) sets 120 and 150 BPM; (4) MIDI clock reaches the OS while a long SysEx is being sent, on TRS and USB
// (the tempo the OS measures follows a change from 120 to 150 BPM made during the SysEx; it measures incoming clock
// even with clock receive off, as with factory settings); (5) the OS never enters its exception handler. Exits 1 on
// any failure.
//   mctiming_test <main_os.bin> <flash.bin>   (on a copy of the flash and a fresh +Drive in a temporary folder)
// DECAY's address is found by setting it over MIDI CC 80 (it differs between projects); the tempo is the 16-bit
// BPM x 120 at 0x404ed7c2 (OS 1.13); the incoming clock is the sum of its last 24 intervals in 135.168 MHz / 512
// ticks at 0x40fb57d4 (read by 0x40080618, Modded-Cycles' OS map).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mcboard.h"

#define DDR_BASE 0x40000000u
#define DDR_LEN 0x08000000u
#define TEMPO_ADDR 0x404ed7c2u
#define CLOCK_SUM_ADDR 0x40fb57d4u
#define DECAY 3                                       // OS encoder number

static mcboard *B;
static int failures;
static uint32_t decay_addr;

static void check(int ok, const char *what, long got, const char *want) {
    printf("%s  %-46s got %ld, want %s\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) failures++;
}

static void cc_decay(int v) { uint8_t m[] = {0xB9, 80, (uint8_t)v}; mcb_midi_in(B, m, 3); mcb_run(B, 0.3); }
static int decay(void) { uint8_t v = 0; mcb_peek(B, decay_addr, &v, 1); return v; }
static int tempo_x10(void) {
    uint8_t t[2]; mcb_peek(B, TEMPO_ADDR, t, 2);
    return ((t[0] << 8 | t[1]) + 6) / 12;
}

// The tempo the OS measures from incoming MIDI clock, BPM x 10 (0 before any clock).
static int clock_bpm_x10(void) {
    uint8_t t[4]; mcb_peek(B, CLOCK_SUM_ADDR, t, 4);
    uint32_t sum = (uint32_t)t[0] << 24 | t[1] << 16 | t[2] << 8 | t[3];
    return sum ? (int)lround(1900800000.0 * 512 / 12 / sum) : 0;
}

// MIDI clock at `bpm` for `secs`, paced on the wall clock (mcb_run is real time), into the TRS or USB port; with a
// SysEx, it is fed alongside as the app does (what the board does not take yet is sent again), from *sent on.
static size_t (*const midi_in[2])(mcboard *, const uint8_t *, size_t) = {mcb_midi_in, mcb_usb_midi_in};
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static void clock_for(int usb, double bpm, double secs, const uint8_t *sx, size_t len, size_t *sent) {
    const uint8_t c = 0xF8;
    double dt = 60.0 / bpm / 24, t0 = now_s();
    for (int k = 1; k * dt <= secs; k++) {
        if (sx && *sent < len) *sent += midi_in[usb](B, sx + *sent, len - *sent);
        midi_in[usb](B, &c, 1);
        double left = t0 + k * dt - now_s();
        if (left > 0) mcb_run(B, left);
    }
}

static void clock_during_sysex(int usb) {
    static uint8_t sx[20000];                             // 6.4 s at 31250 baud; maker 7D (non-commercial): ignored
    sx[0] = 0xF0; sx[1] = 0x7D;
    for (size_t i = 2; i < sizeof sx - 1; i++) sx[i] = i & 0x7F;
    sx[sizeof sx - 1] = 0xF7;
    const char *port = usb ? "USB" : "TRS";
    char what[80];
    size_t sent = 0;
    clock_for(usb, 120, 2, NULL, 0, NULL);
    snprintf(what, sizeof what, "%s: clock at 120 BPM, as the OS measures it", port);
    check(abs(clock_bpm_x10() - 1200) <= 15, what, clock_bpm_x10(), "1185..1215");
    clock_for(usb, 150, 2, sx, sizeof sx, &sent);
    int during = clock_bpm_x10();
    if (!usb)
        check(sent < sizeof sx, "TRS: the SysEx is still being sent 2 s in", (long)sent, "< 20000 bytes taken");
    snprintf(what, sizeof what, "%s: clock changed to 150 during the SysEx", port);
    check(abs(during - 1500) <= 15, what, during, "1485..1515");
    while (sent < sizeof sx) clock_for(usb, 150, 1, sx, sizeof sx, &sent);
    clock_for(usb, 150, 1, NULL, 0, NULL);                // the last of it goes through
}

// DECAY's byte: the one that follows CC 80 through 10, 77 and 33 (the first such, outside the UI's scratch copies)
static uint32_t find_decay(void) {
    uint8_t *a = malloc(DDR_LEN), *b = malloc(DDR_LEN);
    cc_decay(10); mcb_peek(B, DDR_BASE, a, DDR_LEN);
    cc_decay(77); mcb_peek(B, DDR_BASE, b, DDR_LEN);
    cc_decay(33);
    uint32_t found = 0;
    for (uint32_t i = 0; i < DDR_LEN && !found; i++) {
        if (a[i] != 10 || b[i] != 77) continue;
        uint8_t c; mcb_peek(B, DDR_BASE + i, &c, 1);
        if (c == 33) found = DDR_BASE + i;
    }
    free(a); free(b);
    return found;
}

static void events(int dir, int n, double gap) {
    for (int i = 0; i < n; i++) { mcb_encoder_events(B, DECAY, dir); mcb_run(B, gap); }
}

static void taps(double gap) {
    mcb_key(B, 10, 1); mcb_run(B, 0.05);                  // FUNC + TEMPO: tap tempo
    for (int k = 0; k < 6; k++) { mcb_key(B, 4, 1); mcb_run(B, 0.05); mcb_key(B, 4, 0); mcb_run(B, gap - 0.05); }
    mcb_key(B, 10, 0); mcb_run(B, 1.5);
}

static int copy(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    if (!in || !out) return -1;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    return fclose(out);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s main_os.bin flash.bin\n", argv[0]); return 2; }
    char dir[512], flash[600], emmc[600], extcsd[620];
    snprintf(dir, sizeof dir, "%s/mctiming.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    snprintf(flash, sizeof flash, "%s/flash.bin", dir);
    snprintf(emmc, sizeof emmc, "%s/emmc.img", dir);
    snprintf(extcsd, sizeof extcsd, "%s.extcsd", emmc);
    if (copy(argv[2], flash)) { fprintf(stderr, "cannot copy %s\n", argv[2]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *os = malloc(n);
    if (fread(os, 1, n, f) != n) return 2;
    fclose(f);
    char err[256];
    B = mcb_create(os, n, flash, emmc, err, sizeof err);
    if (!B) { fprintf(stderr, "%s\n", err); return 2; }
    mcb_run(B, 6);

    decay_addr = find_decay();
    check(decay_addr != 0, "DECAY found in RAM (MIDI CC 80)", decay_addr != 0, "1");
    if (!decay_addr) { mcb_destroy(B); unlink(emmc); unlink(extcsd); unlink(flash); rmdir(dir); return 1; }
    mcb_run(B, 40 - 8);                                   // past the first wrap of timer 0 (32 s)

    uint32_t c0 = mcb_timer_count(B, 0); mcb_run(B, 0.1); uint32_t c1 = mcb_timer_count(B, 0);
    long ticks = (long)(uint32_t)(c1 - c0);
    check(ticks > 12000000 && ticks < 16000000, "DMA timer 0 ticks in 0.1 s at 40 s", ticks, "~13.5 million");

    cc_decay(50); mcb_run(B, 1);
    events(1, 1, 1.0);
    check(decay() == 50, "a lone knob event after a pause: no change", decay(), "50");
    for (int k = 0; k < 4; k++) { events(1, 1, 0.04); events(1, 1, 0.6); }   // clicks (2 events, 40 ms apart)
    int clicks = decay() - 50;
    check(clicks >= 3 && clicks <= 6, "4 clicks 0.6 s apart: about one step each", clicks, "3..6");
    int start = decay();
    events(1, 10, 0.05); mcb_run(B, 1);
    int up = decay() - start;
    check(up >= 8 && up <= 10, "10 events 50 ms apart: about one step each", up, "8..10");
    int top = decay();
    events(-1, 10, 0.05); mcb_run(B, 1);
    int down = top - decay();
    check(down >= up - 1 && down <= up + 1, "the same turn back: as far", down, "within 1 of the way up");
    for (int i = 0; i < 200; i++) { mcb_encoder(B, DECAY, 3); mcb_run(B, 0.002); }   // a fast scroll up
    int spun = decay();
    events(-1, 10, 0.05); mcb_run(B, 1);
    check(decay() < spun, "a fast spin, then turned back: it goes down", spun - decay(), "> 0 steps down");

    taps(0.5);
    check(abs(tempo_x10() - 1200) <= 10, "tap tempo, 0.5 s taps (BPM x 10)", tempo_x10(), "1190..1210");
    taps(0.4);
    check(abs(tempo_x10() - 1500) <= 10, "tap tempo, 0.4 s taps (BPM x 10)", tempo_x10(), "1490..1510");

    clock_during_sysex(0);
    clock_during_sysex(1);

    check(mcb_os_exceptions(B) == 0, "OS exceptions", (long)mcb_os_exceptions(B), "0");
    mcb_destroy(B);
    unlink(emmc); unlink(extcsd); unlink(flash); rmdir(dir);
    printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
