// For each key index: fresh boot, press once, save screen + LED changes.  mckeyscan <os> <flash> <emmc> <outdir> idx...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "mcboard.h"
static void put32(FILE *f, uint32_t v) { fputc(v >> 24, f); fputc(v >> 16 & 255, f); fputc(v >> 8 & 255, f); fputc(v & 255, f); }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) { put32(f, n); uLong c = crc32(0, (const Bytef *)t, 4); fwrite(t, 1, 4, f); if (n) { fwrite(d, 1, n, f); c = crc32(c, d, n); } put32(f, (uint32_t)c); }
static void png(const char *path, const uint8_t fb[1024]) {
    int s = 2, w = 128 * s, h = 64 * s; size_t rl = (size_t)(w + 1) * h; uint8_t *raw = malloc(rl);
    for (int r = 0; r < h; r++) { uint8_t *row = raw + (size_t)r * (w + 1); row[0] = 0; int y = 63 - r / s;
        for (int x = 0; x < w; x++) row[1 + x] = (fb[(x / s) * 8 + y / 8] >> (7 - (y & 7)) & 1) ? 0x20 : 0xE8; }
    uLongf zl = compressBound(rl); uint8_t *z = malloc(zl); compress(z, &zl, raw, rl);
    uint8_t ih[13] = {0, 0, w >> 8, w & 255, 0, 0, h >> 8, h & 255, 8, 0, 0, 0, 0};
    FILE *f = fopen(path, "wb"); fwrite("\x89PNG\r\n\x1a\n", 1, 8, f); chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)zl); chunk(f, "IEND", NULL, 0); fclose(f); free(raw); free(z);
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *os = malloc(n); if (fread(os, 1, n, f) != n) return 1; fclose(f);
    for (int a = 5; a < argc; a++) {
        int k = atoi(argv[a]); char err[256], p[512]; uint8_t fb[1024];
        mcboard *b = mcb_create(os, n, argv[2], argv[3], err, sizeof err);
        mcb_run(b, 5);
        uint64_t l0 = mcb_leds(b);
        mcb_key(b, k, 1); mcb_run(b, getenv("HOLD") ? 0.6 : 0.25); if (!getenv("HOLD")) { mcb_key(b, k, 0); mcb_run(b, 0.4); }
        mcb_screen(b, fb); snprintf(p, sizeof p, "%s/key%02d.png", argv[4], k); png(p, fb);
        uint64_t l = mcb_leds(b);
        printf("key %2d:", k); for (int i = 0; i < 56; i++) if ((l ^ l0) >> i & 1) printf(" %c%d", (l >> i & 1) ? '+' : '-', i); printf("\n"); fflush(stdout);
        mcb_destroy(b);
    }
    return 0;
}
