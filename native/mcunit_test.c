// Unit checks of the board's own logic, without the OS: no firmware needed, so they run anywhere (ok/FAIL).
//   mcunit_test
// It includes mcboard.c to reach its internals. A board that is never run needs no OS code: mcb_create gets a few
// zero bytes as its "OS".
#include "mcboard.c"

static int failures;

static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static char dir[512];

static void path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

static void write_file(const char *name, size_t size) {
    char p[600];
    path(p, sizeof p, name);
    FILE *f = fopen(p, "wb");
    uint8_t *z = calloc(1, size + 1);
    fwrite(z, 1, size, f);
    fclose(f);
    free(z);
}

// mcb_create with a flash image of `size` bytes; returns the board (or NULL) and its error text.
static mcboard *create(size_t size, char *err, size_t errlen) {
    static const uint8_t os[16] = {0};
    char flash[600], emmc[600];
    write_file("flash.bin", size);
    path(flash, sizeof flash, "flash.bin");
    path(emmc, sizeof emmc, "emmc.img");
    err[0] = 0;
    return mcb_create(os, sizeof os, flash, emmc, err, errlen);
}

static void test_flash_size(void) {
    char err[256];
    mcboard *b = create(0, err, sizeof err);
    check(b == NULL && strstr(err, "flash"), "an empty flash.bin is refused with a message");
    mcb_destroy(b);
    b = create(MCB_FLASH_SIZE / 2, err, sizeof err);
    check(b == NULL && strstr(err, "flash"), "a flash.bin of the wrong size is refused");
    mcb_destroy(b);
    b = create(MCB_FLASH_SIZE, err, sizeof err);
    check(b != NULL, "a 2 MB flash.bin is taken");
    mcb_destroy(b);
}

// USB-MIDI event packets the host side queued (host -> device), as "cin b1 b2 b3" words.
static int events(mcboard *b, uint32_t *out, int max) {
    int n = 0;
    for (uint64_t r = b->usb.ev_r; r != b->usb.ev_w && n < max; r++, n++) {
        const uint8_t *e = b->usb.ev + 4 * (r & 4095);
        out[n] = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3];
    }
    b->usb.ev_r = b->usb.ev_w;
    return n;
}

static int same(mcboard *b, const uint8_t *in, size_t n, const uint32_t *want, int nwant) {
    uint32_t got[64];
    mcb_usb_midi_in(b, in, n);
    int k = events(b, got, 64);
    if (k != nwant) return 0;
    for (int i = 0; i < k; i++) if (got[i] != want[i]) return 0;
    return 1;
}

static void test_usb_midi_parser(void) {
    mcboard *b = calloc(1, sizeof *b);
    {
        static const uint8_t in[] = {0x90, 60, 100, 62, 90};               // running status
        static const uint32_t want[] = {0x09903c64, 0x09903e5a};
        check(same(b, in, sizeof in, want, 2), "note on, then running status");
    }
    {
        static const uint8_t in[] = {0x90, 60, 0xF8, 100};                 // clock inside a message
        static const uint32_t want[] = {0x0FF80000, 0x09903c64};
        check(same(b, in, sizeof in, want, 2), "a real-time byte inside a message passes on its own");
    }
    {
        static const uint8_t in[] = {0xF0, 1, 2, 3, 4, 0xF7};              // 6 bytes: start, end with 3
        static const uint32_t want[] = {0x04F00102, 0x070304F7};
        check(same(b, in, sizeof in, want, 2), "SysEx of 6 bytes: CIN 4, then CIN 7");
    }
    {
        static const uint8_t in[] = {0xF0, 1, 2, 0xF7};                    // ends on a packet boundary
        static const uint32_t want[] = {0x04F00102, 0x05F70000};
        check(same(b, in, sizeof in, want, 2), "SysEx ending right after a full packet: CIN 5 with F7");
    }
    {
        static const uint8_t in[] = {0xF0, 1, 2, 0x90, 60, 100};           // cut off by a status byte
        static const uint32_t want[] = {0x04F00102, 0x09903c64};
        check(same(b, in, sizeof in, want, 2), "SysEx cut off on a packet boundary sends no stale packet");
    }
    {
        static const uint8_t in[] = {0xF0, 1, 0x80, 60, 0};                // cut off mid-packet
        static const uint32_t want[] = {0x06F00100, 0x08803c00};
        check(same(b, in, sizeof in, want, 2), "SysEx cut off mid-packet ends with what it has");
    }
    {
        static const uint8_t in[] = {0xC5, 7, 0xF2, 1, 2, 0xF6};           // program change, song position, tune
        static const uint32_t want[] = {0x0CC50700, 0x03F20102, 0x05F60000};
        check(same(b, in, sizeof in, want, 3), "program change, song position, tune request");
    }
    free(b);
}

int main(void) {
    snprintf(dir, sizeof dir, "%s/mcunit.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_flash_size();
    test_usb_midi_parser();
    char p[600];
    static const char *names[] = {"flash.bin", "emmc.img", "emmc.img.extcsd"};
    for (int i = 0; i < 3; i++) { path(p, sizeof p, names[i]); unlink(p); }
    rmdir(dir);
    printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
