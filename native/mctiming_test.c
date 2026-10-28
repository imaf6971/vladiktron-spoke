// Headless check of the timing the OS sees, run past the 32 s at which DMA timer 0's 32-bit counter first wraps:
// (1) DMA timer 0 keeps counting at 135.168 MHz; (2) knob events (OS knob events, as the MiniLab mapping in
// app/MiniLab.swift sends them; the OS stamps them with timer 0): a lone event after a pause changes nothing, a click
// (two events 40 ms apart) is about one step, steady events about one step each, and both ways alike (the ranges
// are what OS 1.13 does; the OS applies steps on its own tick and speeds up on fast turns); (3) tap tempo
// (FUNC + TEMPO) sets 120 and 150 BPM; (4) the OS never enters its exception handler. Exits 1 on any failure.
//   mctiming_test <main_os.bin> <flash.bin>   (on a copy of the flash and a fresh +Drive in a temporary folder)
// DECAY's address is found by setting it over MIDI CC 80 (it differs between projects); the tempo is the 16-bit
// BPM x 120 at 0x404ed7c2 (OS 1.13).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mcboard.h"

#define DDR_BASE 0x40000000u
#define DDR_LEN 0x08000000u
#define TEMPO_ADDR 0x404ed7c2u
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

    taps(0.5);
    check(abs(tempo_x10() - 1200) <= 10, "tap tempo, 0.5 s taps (BPM x 10)", tempo_x10(), "1190..1210");
    taps(0.4);
    check(abs(tempo_x10() - 1500) <= 10, "tap tempo, 0.4 s taps (BPM x 10)", tempo_x10(), "1490..1510");

    check(mcb_os_exceptions(B) == 0, "OS exceptions", (long)mcb_os_exceptions(B), "0");
    mcb_destroy(B);
    unlink(emmc); unlink(extcsd); unlink(flash); rmdir(dir);
    printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
