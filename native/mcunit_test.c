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

// mcb_create with a flash image of `size` bytes and `os` (zeros if NULL); returns the board (or NULL) and its error.
static mcboard *create_os(const uint8_t *os, size_t len, size_t size, char *err, size_t errlen) {
    static const uint8_t zeros[16] = {0};
    if (!os) { os = zeros; len = sizeof zeros; }
    char flash[600], emmc[600];
    write_file("flash.bin", size);
    path(flash, sizeof flash, "flash.bin");
    path(emmc, sizeof emmc, "emmc.img");
    err[0] = 0;
    return mcb_create(os, len, flash, emmc, err, errlen);
}
static mcboard *create(size_t size, char *err, size_t errlen) { return create_os(NULL, 0, size, err, errlen); }

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

// An access to unmapped memory stops the CPU as an unmapped read (the hook answers "not handled"), with the fault.
static void test_unmapped_access(void) {
    uint8_t os[0x100] = {0};
    static const uint8_t code[] = {0x20, 0x39, 0x20, 0x00, 0x00, 0x00};   // move.l (0x20000000).l,d0
    memcpy(os + (OS_ENTRY - OS_BASE), code, sizeof code);
    char err[256];
    mcboard *b = create_os(os, sizeof os, MCB_FLASH_SIZE, err, sizeof err);
    if (!b) { check(0, err); return; }
    uc_err r = uc_emu_start(b->uc, OS_ENTRY, 0xFFFFFFFFull, 0, 4);
    check(r == UC_ERR_READ_UNMAPPED, "an unmapped read stops the CPU with UC_ERR_READ_UNMAPPED");
    check(strstr(b->fault, "unmapped access 0x20000000") != NULL, "... and the fault names the address");
    mcb_destroy(b);
}

// MIDI in, both ports: what does not fit is not taken (the caller keeps it and sends it again), never dropped.
static void test_midi_in_backpressure(void) {
    mcboard *b = calloc(1, sizeof *b);
    static uint8_t in[20000], out[20000];
    for (size_t i = 0; i < sizeof in; i++) in[i] = (uint8_t)(i & 0x7F);
    size_t took = mcb_midi_in(b, in, sizeof in);
    check(took == MIDI_RING, "TRS: a full ring takes no more (4 KB taken of 20 KB)");
    int same = 1;
    for (size_t i = 0; i < took; i++) same &= b->midi_in[i & (MIDI_RING - 1)] == in[i];
    b->min_r += 1000;                                         // the OS read 1000 bytes
    took = mcb_midi_in(b, in + MIDI_RING, sizeof in - MIDI_RING);
    check(same && took == 1000, "TRS: the bytes taken are in order, and more go in as the OS reads");
    free(b);

    b = calloc(1, sizeof *b);                                 // USB: a 20 KB SysEx through 4096 event packets
    in[0] = 0xF0; in[sizeof in - 1] = 0xF7;
    size_t off = 0, o = 0, first = 0;
    for (int round = 0; off < sizeof in && round < 100; round++) {
        size_t k = mcb_usb_midi_in(b, in + off, sizeof in - off);
        if (!round) first = k;
        off += k;
        for (uint64_t r = b->usb.ev_r; r != b->usb.ev_w; r++) {   // the device takes every packet
            const uint8_t *e = b->usb.ev + 4 * (r & 4095);
            int len = e[0] == 4 || e[0] == 7 ? 3 : e[0] == 6 ? 2 : e[0] == 5 ? 1 : 0;
            for (int j = 0; j < len && o < sizeof out; j++) out[o++] = e[1 + j];
        }
        b->usb.ev_r = b->usb.ev_w;
    }
    check(first < sizeof in, "USB: a full packet queue takes no more bytes");
    check(off == sizeof in && o == sizeof in && memcmp(in, out, sizeof in) == 0,
          "USB: the whole SysEx arrives, in order, once the device takes the packets");
    free(b);
}

// SPI flash writes (the OS saving its settings) reach flash.bin in one save shortly after, not one 2 MB save per
// command; whatever is still unsaved is saved when the board is destroyed.
static void spi(mcboard *b, uint8_t v, int more) {         // one byte through DSPI0 PUSHR (CONT = chip select stays)
    mmio_write(b->uc, 0x5C034, 4, (more ? 1u << 31 : 0) | v, mmio_ctx[1]);
    b->dspi0_n = 0;                                           // the OS reads each answer back (POPR)
}
static void spi_cmd(mcboard *b, uint8_t cmd, uint32_t addr, int data_bytes) {
    spi(b, 0x06, 0);                                          // write enable
    spi(b, cmd, 1);
    spi(b, (uint8_t)(addr >> 16), 1); spi(b, (uint8_t)(addr >> 8), 1); spi(b, (uint8_t)addr, data_bytes > 0);
    for (int i = 0; i < data_bytes; i++) spi(b, 0x00, i + 1 < data_bytes);
}
static int flash_file_zero(uint32_t at, size_t n) {
    char p[600]; path(p, sizeof p, "flash.bin");
    FILE *f = fopen(p, "rb"); uint8_t buf[4096]; int zero = 1;
    if (!f || fseek(f, at, SEEK_SET) || fread(buf, 1, n, f) != n) zero = 0;
    for (size_t i = 0; zero && i < n; i++) zero = buf[i] == 0;
    if (f) fclose(f);
    return zero;
}
static void test_flash_save_batched(void) {
    char err[256];
    mcboard *b = create(MCB_FLASH_SIZE, err, sizeof err);    // (the image is all zeros: the erase makes it FF)
    if (!b) { check(0, err); return; }
    spi_cmd(b, 0x20, 0x1F0000, 0);                            // erase a 4 KB sector, then program it, 16 pages
    for (uint32_t k = 0; k < 16; k++) spi_cmd(b, 0x02, 0x1F0000 + 256 * k, 256);
    check(b->n_flash_saves == 0, "17 flash commands: flash.bin is not saved at once");
    flash_service(b, now_us() - b->t0 + 1e6);                 // a second later, between CPU slices
    check(b->n_flash_saves == 1 && flash_file_zero(0x1F0000, 4096), "... but once, a moment later, with all of it");
    spi_cmd(b, 0x20, 0x1F1000, 0);                            // one more sector, then the board goes
    mcb_destroy(b);
    check(!flash_file_zero(0x1F1000, 16), "what is still unsaved is saved when the board is destroyed");
}

// mcb_busy_seconds: how long the CPU has run without reaching the OS's idle loop. A hung OS keeps growing it; one
// that waits in its idle loop keeps it near zero.
static void test_busy_seconds(void) {
    static uint8_t os[IDLE_PC - OS_BASE + 2];
    static const uint8_t spin[] = {0x60, 0xFE};                               // bra.s . (not the idle loop)
    char err[256];
    memcpy(os + (OS_ENTRY - OS_BASE), spin, sizeof spin);
    mcboard *b = create_os(os, sizeof os, MCB_FLASH_SIZE, err, sizeof err);
    if (!b) { check(0, err); return; }
    mcb_run(b, 1.0);
    double busy = mcb_busy_seconds(b);
    check(busy > 0.8 && busy < 1.5, "a CPU spinning outside the idle loop for 1 s is busy for about 1 s");
    mcb_destroy(b);
    memset(os, 0, sizeof os);
    static const uint8_t to_idle[] = {0x4E, 0xF9, IDLE_PC >> 24, IDLE_PC >> 16 & 0xFF, IDLE_PC >> 8 & 0xFF, IDLE_PC & 0xFF};
    memcpy(os + (OS_ENTRY - OS_BASE), to_idle, sizeof to_idle);              // jmp IDLE_PC
    memcpy(os + (IDLE_PC - OS_BASE), spin, sizeof spin);                      // the idle loop: bra.s .
    b = create_os(os, sizeof os, MCB_FLASH_SIZE, err, sizeof err);
    if (!b) { check(0, err); return; }
    mcb_run(b, 1.0);
    busy = mcb_busy_seconds(b);
    check(busy < 0.05, "a CPU waiting in the idle loop is not busy");
    mcb_destroy(b);
}

// mcb_fault says why the CPU stopped, once mcb_run has returned; a healthy board has no fault.
static void test_fault_report(void) {
    uint8_t os[0x100] = {0};
    static const uint8_t code[] = {0x20, 0x39, 0x20, 0x00, 0x00, 0x00};   // move.l (0x20000000).l,d0
    char err[256], why[256] = "untouched";
    mcboard *b = create(MCB_FLASH_SIZE, err, sizeof err);
    if (!b) { check(0, err); return; }
    check(mcb_fault(b, why, sizeof why) == 0 && why[0] == 0, "a board that has not faulted reports no fault");
    mcb_destroy(b);
    memcpy(os + (OS_ENTRY - OS_BASE), code, sizeof code);
    b = create_os(os, sizeof os, MCB_FLASH_SIZE, err, sizeof err);
    if (!b) { check(0, err); return; }
    int r = mcb_run(b, 0.5);
    check(r != 0 && mcb_fault(b, why, sizeof why) == 1 && strstr(why, "unmapped access 0x20000000"),
          "after a CPU fault mcb_run returns nonzero and mcb_fault says why");
    mcb_destroy(b);
}

// A playback microframe never writes more than its packet buffer holds, whatever the OS's dTD and the backlog.
static void test_usb_play_packet_bound(void) {
    char err[256];
    mcboard *b = create(MCB_FLASH_SIZE, err, sizeof err);
    if (!b) { check(0, err); return; }
    uint32_t eplist = 0x80000000u, dtd = 0x40100000u, data = 0x40200000u;
    b->usb.eplist = eplist;
    b->usb.play_ep = 1;
    b->usb.play_on = b->usb.play_alt = 1;
    b->usb.fb = 6.0;
    b->usb.play_acc = 1000;                                   // a backlog of frames owed
    gwrite32(b, dtd, 1);                                      // next: none
    gwrite32(b, dtd + 4, 0x7FFFu << 16 | 0x80);               // 32 KB buffer, active
    for (uint32_t i = 0; i < 5; i++) gwrite32(b, dtd + 8 + 4 * i, data + 0x1000 * i);
    b->usb.cur[2] = dtd;                                      // endpoint 1 OUT
    usb_microframe(b);
    check(b->usb.n_play > 0 && b->usb.n_play <= 128, "a playback packet holds at most 128 frames (1 KB)");
    check(b->usb.play_acc < 16, "... and the backlog does not keep growing");
    mcb_destroy(b);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);                        // what passed shows even if a check crashes
    snprintf(dir, sizeof dir, "%s/mcunit.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_flash_size();
    test_usb_midi_parser();
    test_unmapped_access();
    test_usb_play_packet_bound();
    test_fault_report();
    test_midi_in_backpressure();
    test_flash_save_batched();
    test_busy_seconds();
    char p[600];
    static const char *names[] = {"flash.bin", "emmc.img", "emmc.img.extcsd"};
    for (int i = 0; i < 3; i++) { path(p, sizeof p, names[i]); unlink(p); }
    rmdir(dir);
    printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
