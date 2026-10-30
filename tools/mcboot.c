// Headless runner for the virtual board: boots the OS for N seconds, prints status, saves the screen.
//   mcboot <main_os.bin> <flash.bin> <emmc.img> [seconds] [screen.png]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "mcboard.h"

static void put32(FILE *f, uint32_t v) { fputc(v >> 24, f); fputc(v >> 16 & 255, f); fputc(v >> 8 & 255, f); fputc(v & 255, f); }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    put32(f, n);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    fwrite(type, 1, 4, f);
    if (n) { fwrite(d, 1, n, f); crc = crc32(crc, d, n); }
    put32(f, (uint32_t)crc);
}
static void write_png(const char *path, const uint8_t fb[1024], int scale) {
    int w = 128 * scale, h = 64 * scale;
    size_t rawlen = (size_t)(w + 1) * h;
    uint8_t *raw = malloc(rawlen);
    for (int r = 0; r < h; r++) {
        uint8_t *row = raw + (size_t)r * (w + 1);
        row[0] = 0;
        int y = 63 - r / scale;
        for (int x = 0; x < w; x++) {
            int px = x / scale;
            row[1 + x] = (fb[px * 8 + y / 8] >> (7 - (y & 7)) & 1) ? 0x20 : 0xE8;
        }
    }
    uLongf zlen = compressBound(rawlen);
    uint8_t *z = malloc(zlen);
    compress(z, &zlen, raw, rawlen);
    uint8_t ihdr[13] = {0};
    ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
    ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
    ihdr[8] = 8;
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zlen);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: mcboot <main_os.bin> <flash.bin> <emmc.img> [seconds] [screen.png]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    size_t len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *os = malloc(len);
    if (fread(os, 1, len, f) != len) return 1;
    fclose(f);
    char err[256];
    mcboard *b = mcb_create(os, len, argv[2], argv[3], err, sizeof err);
    free(os);
    if (!b) { fprintf(stderr, "board: %s\n", err); return 1; }
    double secs = argc > 4 ? atof(argv[4]) : 10;
    char st[512];
    for (double t = 0; t < secs; t += 1) {
        int r = mcb_run(b, 1.0);
        mcb_status(b, st, sizeof st);
        printf("%s\n", st);
        fflush(stdout);
        if (r) break;
    }
    if (argc > 5) {
        uint8_t fb[1024];
        mcb_screen(b, fb);
        int lit = 0;
        for (int i = 0; i < 1024; i++) lit += __builtin_popcount(fb[i]);
        write_png(argv[5], fb, 4);
        printf("screen: %d lit pixels -> %s\n", lit, argv[5]);
    }
    mcb_destroy(b);
    return 0;
}
