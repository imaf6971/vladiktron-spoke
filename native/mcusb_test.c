// Headless USB check, with the board on its own thread as in the app:
//   1. the built-in host enumerates the OS's USB device and USB-MIDI notes play;
//   2. USB audio playback (computer -> Model:Cycles): a pitch sweep sent in comes out of the main output, sample
//      for sample, at the OS's fixed USB-in gain;
//   3. USB audio capture (Model:Cycles -> computer) is the main mix, sample for sample, while a pattern plays.
//   mcusb_test <main_os.bin> <flash.bin> <emmc.img> [seconds]
// Prints PASS/FAIL per check; exit status 0 only if all pass.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mcboard.h"

#define MAXF (48000 * 40)
static float *mainout, *cap;
static size_t nmain, ncap, nsent;
static int failures;

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static double sweep(size_t i) { double t = i / 48000.0; return 0.5 * sin(2 * M_PI * (300 * t + 400 * t * t)); }
static void *cpu(void *b) { mcb_run(b, 0); return NULL; }
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; }

// Moves audio for `secs` like an app would, every 2 ms: main out and USB capture in, the sweep out over USB.
static void pump(mcboard *b, double secs, int feed) {
    float tmp[2 * 4096];
    for (double end = now() + secs; now() < end; ) {
        usleep(2000);
        size_t k;
        while ((k = mcb_audio(b, tmp, 4096)) > 0) if (nmain + k < MAXF) { memcpy(mainout + 2 * nmain, tmp, k * 8); nmain += k; }
        while ((k = mcb_usb_audio_read(b, tmp, 4096)) > 0) if (ncap + k < MAXF) { memcpy(cap + 2 * ncap, tmp, k * 8); ncap += k; }
        while (feed && mcb_usb_audio_write_queued(b) < 960) {
            float f[2 * 48];
            for (int i = 0; i < 48; i++) { float v = (float)sweep(nsent + (size_t)i); f[2 * i] = v; f[2 * i + 1] = -v; }
            mcb_usb_audio_write(b, f, 48);
            nsent += 48;
        }
    }
}
static double peak(const float *x, size_t n) { double p = 0; for (size_t i = 0; i < 2 * n; i++) if (fabs(x[i]) > p) p = fabs(x[i]); return p; }

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    double secs = argc > 4 ? atof(argv[4]) : 8;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *os = malloc(n); if (fread(os, 1, n, f) != n) return 1; fclose(f);
    char err[256], st[700];
    mcboard *b = mcb_create(os, n, argv[2], argv[3], err, sizeof err);
    if (!b) { fprintf(stderr, "%s\n", err); return 1; }
    mainout = malloc(sizeof(float) * 2 * MAXF);
    cap = malloc(sizeof(float) * 2 * MAXF);
    pthread_t t;
    pthread_create(&t, NULL, cpu, b);
    usleep(7000000);                                          // boot and enumerate

    // 1. link and USB-MIDI
    check(mcb_usb_state(b) == 2, "USB: enumerated and configured");
    nmain = 0;
    uint8_t on[3] = {0x90, 60, 110}, off[3] = {0x80, 60, 0};
    mcb_usb_midi_in(b, on, 3); pump(b, 0.3, 0); mcb_usb_midi_in(b, off, 3);
    printf("      USB-MIDI note on channel 1: peak %.3f\n", peak(mainout, nmain));
    check(peak(mainout, nmain) > 0.05, "USB-MIDI: a note sent over USB plays track 1");
    pump(b, 3.0, 0);                                         // until it has rung out

    // 2. playback: a sweep in over USB, the main output must follow it exactly (nothing else playing)
    mcb_usb_audio_enable(b, 0, 1);
    pump(b, 0.5, 0);
    nmain = 0;
    pump(b, secs / 2, 1);
    size_t s0 = 0;
    while (s0 < nmain && fabs(mainout[2 * s0]) < 0.003) s0++;
    double best = 1e30, g = 0;
    long at = 0;
    for (long l = 0; l < 4000 && s0 + 2000 < nmain; l++) {
        double num = 0, den = 0, e = 0;
        for (int i = 0; i < 2000; i++) { double x = sweep((size_t)(l + i)); num += mainout[2 * (s0 + (size_t)i)] * x; den += x * x; }
        for (int i = 0; i < 2000; i++) { double d = mainout[2 * (s0 + (size_t)i)] - num / den * sweep((size_t)(l + i)); e += d * d; }
        if (e < best) { best = e; at = l; g = num / den; }
    }
    size_t follow = 0, frames = 0, first_bad = 0;
    for (size_t i = s0; i < nmain && at + (long)(i - s0) + 8 < (long)nsent; i++, frames++) {
        double x = sweep((size_t)(at + (long)(i - s0)));
        int ok = fabs(mainout[2 * i] - g * x) < 2e-4 && fabs(mainout[2 * i + 1] + g * x) < 2e-4;
        follow += ok;
        if (!ok && !first_bad) {
            first_bad = i - s0 + 1;
            printf("      first fault %.4f s in:", (i - s0) / 48000.0);
            for (int k = -2; k < 14; k++) printf(" %+.4f/%+.4f", mainout[2 * (i + (size_t)k)], g * sweep((size_t)(at + (long)(i - s0) + k)));
            printf("\n");
            for (long d = -400; d <= 400; d++)                // where the stream went: a skip or a repeat
                if (d && fabs(mainout[2 * i] - g * sweep((size_t)(at + (long)(i - s0) + d))) < 2e-4 &&
                    fabs(mainout[2 * i + 2] - g * sweep((size_t)(at + (long)(i - s0) + d + 1))) < 2e-4) {
                    printf("      first fault %.3f s in: the stream jumps %+ld frames\n", (i - s0) / 48000.0, d);
                    break;
                }
        }
    }
    printf("      playback: %zu of %zu frames follow the sweep, gain %.2f dB\n", follow, frames, 20 * log10(fabs(g) + 1e-12));
    check(frames > 48000 && follow == frames, "USB audio playback: reaches the main output, sample for sample");
    if (follow != frames) { mcb_status(b, st, sizeof st); printf("      %s\n", st); }

    // 3. capture while the pattern plays
    mcb_usb_audio_enable(b, 1, 0);
    pump(b, 1.0, 0);
    nmain = ncap = 0;
    mcb_key(b, 7, 1); pump(b, 0.1, 0); mcb_key(b, 7, 0);
    pump(b, secs / 2, 0);
    mcb_key(b, 5, 1); pump(b, 0.1, 0); mcb_key(b, 5, 0);
    size_t fm = 0, fc = 0;
    while (fm < nmain && fabs(mainout[2 * fm]) < 0.02) fm++;
    while (fc < ncap && fabs(cap[2 * fc]) < 0.02) fc++;
    long lag = (long)fc - (long)fm;
    size_t same = 0, total = 0;
    for (size_t i = fm; i < nmain && (long)i + lag < (long)ncap; i++, total++)
        same += fabs(cap[2 * (i + lag)] - mainout[2 * i]) < 2e-6 && fabs(cap[2 * (i + lag) + 1] - mainout[2 * i + 1]) < 2e-6;
    printf("      capture: %zu of %zu frames identical to the main mix, %.2f ms behind\n", same, total, lag / 48.0);
    for (size_t i = fm; same != total && i < nmain && (long)i + lag < (long)ncap; i++)
        if (fabs(cap[2 * (i + lag)] - mainout[2 * i]) >= 2e-6) {
            long d;
            for (d = -400; d <= 400; d++)
                if (d && (long)i + lag + d >= 0 && fabs(cap[2 * (i + lag)] - mainout[2 * (i + (size_t)d)]) < 2e-6 &&
                    fabs(cap[2 * (i + lag) + 2] - mainout[2 * (i + (size_t)d) + 2]) < 2e-6) break;
            printf("      first fault %.3f s in, position %zu in its block: capture has the main mix from %+ld frames\n",
                   (i - fm) / 48000.0, (i - fm) % 32, d);
            break;
        }
    check(total > 48000 && same == total, "USB audio capture: the main mix, sample for sample");

    mcb_status(b, st, sizeof st);
    printf("%s\n", st);
    mcb_stop(b);
    pthread_join(t, NULL);
    mcb_destroy(b);
    return failures ? 1 : 0;
}
