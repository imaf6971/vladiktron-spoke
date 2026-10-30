// Prototype driver for the native Model:Cycles engine.
//   mcbench scenario <main_os.bin> <out.raw>   fixed scenario, raw int32 [blocks][6][32] (for exactness checks)
//   mcbench bench    <main_os.bin> [seconds]   6 voices retriggered on every 16th note, timed
//   mcbench demo     <main_os.bin> <out.wav>   4-bar pattern mixed to a stereo WAV
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mcengine.h"

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) { perror(path); exit(1); }
    fclose(f);
    return b;
}

static mc_engine *open_engine(const char *path) {
    size_t len;
    uint8_t *img = read_file(path, &len);
    char err[256];
    mc_engine *e = mc_create(img, len, err, sizeof err);
    free(img);
    if (!e) { fprintf(stderr, "engine: %s\n", err); exit(1); }
    for (int t = 0; t < MC_TRACKS; t++) mc_machine_defaults(e, t, (mc_machine)t);
    return e;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Same scenario as ref_scenario.py; keep the two in sync.
static void scenario_step(mc_engine *e, int b, uint32_t *trig, uint32_t *rel) {
    *trig = *rel = 0;
    if (b == 0) *trig = 0x3f;
    if (b == 120) {
        for (int t = 0; t < MC_TRACKS; t++) {
            static const double color0[6] = {10, 0, 46, 38, 40, 3}, shape0[6] = {16, 127, 48, 100, 38, 43};
            mc_set(e, t, MC_P_PITCH, 54 + 5 * t);
            mc_set(e, t, MC_P_COLOR, (int)(color0[t] + 17) % 128);
            mc_set(e, t, MC_P_SHAPE, (int)(shape0[t] + 31) % 128);
            mc_set(e, t, MC_P_DECAY, 30 + 7 * t);
            mc_set(e, t, MC_P_NOTE, 60 + 2 * t);
        }
        *trig = 0x3f;
    }
    if (b == 200) *rel = 0x3f;
    if (b == 240) {
        for (int t = 0; t < MC_TRACKS; t++) {
            mc_set(e, t, MC_P_SWEEP, 90);
            mc_set(e, t, MC_P_CONTOUR, 10);
            mc_set(e, t, MC_P_PUNCH, 64);
            mc_set(e, t, MC_P_FINETUNE, 70);
        }
        *trig = 0x15;
    }
}

static int cmd_scenario(const char *os, const char *out) {
    mc_engine *e = open_engine(os);
    FILE *f = fopen(out, "wb");
    int32_t buf[MC_TRACKS][MC_FRAMES];
    for (int b = 0; b < 360; b++) {
        uint32_t trig, rel;
        scenario_step(e, b, &trig, &rel);
        if (mc_block(e, trig, rel, buf)) { fprintf(stderr, "fault at block %d\n", b); return 1; }
        fwrite(buf, sizeof buf, 1, f);
    }
    fclose(f);
    mc_destroy(e);
    printf("wrote 360 blocks to %s\n", out);
    return 0;
}

static int cmd_bench(const char *os, double seconds) {
    mc_engine *e = open_engine(os);
    int blocks = (int)(seconds * MC_RATE / MC_FRAMES);
    int32_t buf[MC_TRACKS][MC_FRAMES];
    const int sixteenth = 188;                  // 16th note at 120 BPM, in blocks
    // warm-up: translate the code once
    for (int b = 0; b < 200; b++) mc_block(e, b == 0 ? 0x3f : 0, 0, buf);
    double worst = 0, t0 = now();
    for (int b = 0; b < blocks; b++) {
        double s = now();
        if (mc_block(e, b % sixteenth == 0 ? 0x3f : 0, 0, buf)) { fprintf(stderr, "fault\n"); return 1; }
        double d = now() - s;
        if (d > worst) worst = d;
    }
    double dt = now() - t0;
    double budget = (double)MC_FRAMES / MC_RATE;
    printf("%.1f s of 6-voice audio rendered in %.3f s\n", seconds, dt);
    printf("speed: %.1fx faster than real time (CPU load %.0f%% of one core)\n", seconds / dt, 100 * dt / seconds);
    printf("per 32-frame block: average %.1f us, worst %.1f us, budget %.1f us\n",
           1e6 * dt / blocks, 1e6 * worst, 1e6 * budget);
    mc_destroy(e);
    return 0;
}

static void put16(FILE *f, int v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

static int cmd_demo(const char *os, const char *out) {
    mc_engine *e = open_engine(os);
    // 16-step pattern per track (x = trig), 4 bars at 120 BPM
    static const char *pat[MC_TRACKS] = {
        "x...x...x...x...",   // kick
        "....x.......x...",   // snare
        "..x...x...x...xx",   // metal
        "...x.....x..x...",   // perc
        "x.....x.....x...",   // tone
        "x.......x.......",   // chord
    };
    static const double pan[MC_TRACKS] = {0.5, 0.5, 0.7, 0.3, 0.4, 0.6};
    static const double gain[MC_TRACKS] = {1.0, 0.8, 0.5, 0.7, 0.6, 0.5};
    const int step_blocks = 188, steps = 64;
    int blocks = step_blocks * steps;
    size_t n = (size_t)blocks * MC_FRAMES;
    float *mix = calloc(2 * n, sizeof *mix);
    int32_t buf[MC_TRACKS][MC_FRAMES];
    double t0 = now();
    for (int b = 0; b < blocks; b++) {
        uint32_t trig = 0;
        if (b % step_blocks == 0) {
            int s = (b / step_blocks) % 16;
            for (int t = 0; t < MC_TRACKS; t++)
                if (pat[t][s] == 'x') trig |= 1u << t;
        }
        mc_block(e, trig, 0, buf);
        for (int t = 0; t < MC_TRACKS; t++)
            for (int i = 0; i < MC_FRAMES; i++) {
                double v = buf[t][i] / 2147483648.0 * gain[t];
                mix[2 * ((size_t)b * MC_FRAMES + i)] += (float)(v * (1 - pan[t]));
                mix[2 * ((size_t)b * MC_FRAMES + i) + 1] += (float)(v * pan[t]);
            }
    }
    double dt = now() - t0;
    float peak = 1e-9f;
    for (size_t i = 0; i < 2 * n; i++) if (mix[i] > peak) peak = mix[i]; else if (-mix[i] > peak) peak = -mix[i];
    FILE *f = fopen(out, "wb");
    fwrite("RIFF", 1, 4, f); put32(f, 36 + 4 * n); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, MC_RATE); put32(f, MC_RATE * 4); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, 4 * n);
    for (size_t i = 0; i < 2 * n; i++) put16(f, (int)(mix[i] / peak * 0.89f * 32767));
    fclose(f);
    printf("%.1f s pattern rendered in %.2f s -> %s\n", (double)n / MC_RATE, dt, out);
    free(mix);
    mc_destroy(e);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "scenario")) return cmd_scenario(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "bench")) return cmd_bench(argv[2], argc > 3 ? atof(argv[3]) : 10);
    if (argc >= 4 && !strcmp(argv[1], "demo")) return cmd_demo(argv[2], argv[3]);
    fprintf(stderr, "usage: mcbench scenario|bench|demo <main_os.bin> [out|seconds]\n");
    return 2;
}
