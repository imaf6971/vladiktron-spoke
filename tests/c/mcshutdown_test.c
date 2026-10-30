// Headless clean-shutdown check, on a fresh +Drive in a temporary folder (your own images are not touched):
//   1. boot, change the project (track 1 LEVEL down 8: the level bar on the main screen shows it);
//   2. hold the power key: the OS saves its working state and powers off by itself, within 5 s;
//   3. boot again: the main screen is the one we left (the change was saved), no eMMC errors in either boot;
//   4. control: the same change, then a hard stop (mcb_stop, how the app used to quit), is lost.
//   mcshutdown_test <main_os.bin> <flash.bin>
// Prints PASS/FAIL per check; exit status 0 only if all pass.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mcboard.h"

static unsigned char *os;
static size_t oslen;
static char dir[1024], flash[1100], emmc[1100], extcsd[1110];
static int failures;

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; }
static unsigned long long written(mcboard *b) {         // sectors written so far, from the status line
    char st[700];
    mcb_status(b, st, sizeof st);
    const char *p = strstr(st, " read, ");
    return p ? strtoull(p + 7, NULL, 10) : 0;
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
static mcboard *boot(void) {
    char err[256];
    mcboard *b = mcb_create(os, oslen, flash, emmc, err, sizeof err);
    if (!b) { fprintf(stderr, "board: %s\n", err); exit(1); }
    mcb_run(b, 8);
    return b;
}
// Track 1 LEVEL down 8 detents, then the main screen once the value popup has gone.
static void change(mcboard *b, uint8_t before[1024], uint8_t after[1024]) {
    mcb_screen(b, before);
    mcb_encoder(b, 1, -8);
    mcb_run(b, 3);
    mcb_screen(b, after);
}
static void fresh_drive(const char *flash_src) {
    unlink(emmc);
    unlink(extcsd);
    if (copy(flash_src, flash)) { fprintf(stderr, "cannot copy %s\n", flash_src); exit(1); }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: mcshutdown_test <main_os.bin> <flash.bin>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); oslen = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    os = malloc(oslen);
    if (fread(os, 1, oslen, f) != oslen) return 1;
    fclose(f);
    snprintf(dir, sizeof dir, "%s/mcshutdown.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(flash, sizeof flash, "%s/flash.bin", dir);
    snprintf(emmc, sizeof emmc, "%s/emmc.img", dir);
    snprintf(extcsd, sizeof extcsd, "%s/emmc.img.extcsd", dir);
    uint8_t s0[1024], s1[1024], s2[1024];
    char st[700];

    // 1-2. boot a fresh +Drive, change the project, power off with the key
    fresh_drive(argv[2]);
    mcboard *b = boot();
    change(b, s0, s1);
    check(memcmp(s0, s1, 1024) != 0, "the change shows on the main screen");
    unsigned long long w0 = written(b);
    mcb_power_key(b, 1);
    double t = now();
    mcb_run(b, 5);
    double took = now() - t;
    mcb_status(b, st, sizeof st);
    printf("      %s\n", st);
    printf("      powered off %.2f s after the key went down, %llu sectors written on the way\n", took, written(b) - w0);
    check(mcb_powered_off(b) && took < 4.9, "power key: the OS powers off by itself within 5 s");
    check(written(b) > w0, "power key: the OS saves to the +Drive before it powers off");
    uint64_t errors = mcb_emmc_errors(b);
    mcb_destroy(b);

    // 3. boot again: the project is the one we left
    b = boot();
    mcb_screen(b, s2);
    mcb_status(b, st, sizeof st);
    printf("      %s\n", st);
    check(memcmp(s1, s2, 1024) == 0, "after a reboot the change is still there");
    errors += mcb_emmc_errors(b);
    check(errors == 0, "no eMMC errors in either boot");
    mcb_destroy(b);

    // 4. control: the same, quitting with a hard stop
    fresh_drive(argv[2]);
    b = boot();
    change(b, s0, s1);
    mcb_stop(b);
    mcb_destroy(b);
    b = boot();
    mcb_screen(b, s2);
    check(memcmp(s1, s2, 1024) != 0, "control: after a hard stop the change is lost");
    mcb_destroy(b);

    unlink(emmc); unlink(extcsd); unlink(flash); rmdir(dir);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
