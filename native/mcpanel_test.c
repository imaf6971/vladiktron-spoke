// Scripted front-panel test: boots, then runs key/knob/pad actions and reports screen + LED changes.
//   mcpanel_test <main_os.bin> <flash.bin> <emmc.img> <out_dir>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "mcboard.h"

static void put32(FILE *f, uint32_t v) { fputc(v >> 24, f); fputc(v >> 16 & 255, f); fputc(v >> 8 & 255, f); fputc(v & 255, f); }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    put32(f, n); uLong c = crc32(0, (const Bytef *)t, 4); fwrite(t, 1, 4, f);
    if (n) { fwrite(d, 1, n, f); c = crc32(c, d, n); } put32(f, (uint32_t)c);
}
static void png(const char *path, const uint8_t fb[1024]) {
    int s = 3, w = 128 * s, h = 64 * s; size_t rl = (size_t)(w + 1) * h; uint8_t *raw = malloc(rl);
    for (int r = 0; r < h; r++) { uint8_t *row = raw + (size_t)r * (w + 1); row[0] = 0; int y = 63 - r / s;
        for (int x = 0; x < w; x++) row[1 + x] = (fb[(x / s) * 8 + y / 8] >> (7 - (y & 7)) & 1) ? 0x20 : 0xE8; }
    uLongf zl = compressBound(rl); uint8_t *z = malloc(zl); compress(z, &zl, raw, rl);
    uint8_t ih[13] = {0, 0, w >> 8, w & 255, 0, 0, h >> 8, h & 255, 8, 0, 0, 0, 0};
    FILE *f = fopen(path, "wb"); fwrite("\x89PNG\r\n\x1a\n", 1, 8, f); chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)zl); chunk(f, "IEND", NULL, 0); fclose(f);
    free(raw); free(z);
}
static mcboard *B; static const char *OUT; static int shot;
static uint64_t leds_before;
static void step(const char *what) {
    uint8_t fb[1024]; char p[512];
    mcb_run(B, 0.4);
    mcb_screen(B, fb);
    snprintf(p, sizeof p, "%s/%02d.png", OUT, shot++);
    png(p, fb);
    uint64_t l = mcb_leds(B);
    printf("%-34s -> %s  leds on:", what, p);
    for (int i = 0; i < 56; i++) if (l >> i & 1) printf(" %d", i);
    if (l != leds_before) { printf("  | changed:"); for (int i = 0; i < 56; i++) if ((l ^ leds_before) >> i & 1) printf(" %c%d", (l >> i & 1) ? '+' : '-', i); }
    printf("\n");
    leds_before = l;
}
static void press(int k, const char *name) { mcb_key(B, k, 1); mcb_run(B, 0.12); mcb_key(B, k, 0); step(name); }

int main(int argc, char **argv) {
    if (argc < 5) return 2;
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *os = malloc(n); if (fread(os, 1, n, f) != n) return 1; fclose(f);
    char err[256]; B = mcb_create(os, n, argv[2], argv[3], err, sizeof err); OUT = argv[4];
    if (!B) { fprintf(stderr, "%s\n", err); return 1; }
    mcb_run(B, 6); step("boot");
    mcb_pad(B, 2, 1.0f); mcb_run(B, 0.1); mcb_pad(B, 2, 0); step("pad T2");
    mcb_encoder(B, 3, 12); step("DECAY +12");
    mcb_encoder(B, 2, -5); step("PITCH -5");
    press(7, "PLAY");
    mcb_run(B, 1.0); step("playing 1s");
    press(5, "STOP");
    press(9, "RECORD");
    press(9, "RECORD again");
    press(14, "step 1");
    press(18, "step 5");
    press(11, "PATTERN");
    press(8, "PAD MENU (back)");
    press(13, "TRACK");
    press(30, "PAGE");
    mcb_encoder(B, 1, 3); step("LEVEL/DATA +3");
    char st[512]; mcb_status(B, st, sizeof st); printf("%s\n", st);
    mcb_destroy(B);
    return 0;
}
