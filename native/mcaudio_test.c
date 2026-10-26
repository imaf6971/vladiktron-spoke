// Headless audio check: boots, presses PLAY, records the OS's output to a WAV and reports timing.
//   mcaudio_test <main_os.bin> <flash.bin> <emmc.img> <out.wav> [seconds]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mcboard.h"

static void put16(FILE *f, int v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

int main(int argc, char **argv) {
    if (argc < 5) return 2;
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *os = malloc(n); if (fread(os, 1, n, f) != n) return 1; fclose(f);
    char err[256];
    mcboard *b = mcb_create(os, n, argv[2], argv[3], err, sizeof err);
    if (!b) { fprintf(stderr, "%s\n", err); return 1; }
    double secs = argc > 5 ? atof(argv[5]) : 6;
    float *buf = malloc(sizeof(float) * 2 * 48000 * 60);
    size_t got = 0;
    float tmp[2 * 4096];
    char st[512];
    mcb_run(b, 6);                                   // boot
    while (mcb_audio(b, tmp, 4096)) {}               // drop boot output
    mcb_key(b, 7, 1); mcb_run(b, 0.1); mcb_key(b, 7, 0);   // PLAY
    static const int stepLED[16] = {6, 19, 20, 21, 22, 23, 16, 28, 29, 31, 37, 38, 44, 46, 47, 40};
    printf("playhead every 62.5 ms:");
    for (double t = 0; t < secs; t += 0.0625) {
        mcb_run(b, 0.0625);
        if (t < 2.5) { unsigned long long l = mcb_leds(b); int st = 0;
            for (int i = 0; i < 16; i++) if (l >> stepLED[i] & 1) { st = i + 1; break; }
            printf(" %d", st); }
        size_t k;
        while ((k = mcb_audio(b, tmp, 4096)) > 0 && got + k < 48000 * 60) { memcpy(buf + 2 * got, tmp, k * 8); got += k; }
    }
    mcb_status(b, st, sizeof st);
    printf("\n%s\n", st);
    double peak = 0, sum = 0;
    for (size_t i = 0; i < 2 * got; i++) { double v = fabs(buf[i]); if (v > peak) peak = v; sum += v * v; }
    printf("captured %zu frames (%.2f s of audio in %.1f s), peak %.4f, rms %.5f\n", got, got / 48000.0, secs + 0.1,
           peak, sqrt(sum / (2.0 * (got ? got : 1))));
    FILE *o = fopen(argv[4], "wb");
    fwrite("RIFF", 1, 4, o); put32(o, 36 + got * 4); fwrite("WAVEfmt ", 1, 8, o);
    put32(o, 16); put16(o, 1); put16(o, 2); put32(o, 48000); put32(o, 48000 * 4); put16(o, 4); put16(o, 16);
    fwrite("data", 1, 4, o); put32(o, got * 4);
    double g = peak > 0 ? 0.9 / peak : 1;            // normalised for listening
    for (size_t i = 0; i < 2 * got; i++) put16(o, (int)lrint(buf[i] * g * 32767));
    fclose(o);
    mcb_destroy(b);
    return 0;
}
