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

int main(void) {
    snprintf(dir, sizeof dir, "%s/mcunit.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_flash_size();
    char p[600];
    static const char *names[] = {"flash.bin", "emmc.img", "emmc.img.extcsd"};
    for (int i = 0; i < 3; i++) { path(p, sizeof p, names[i]); unlink(p); }
    rmdir(dir);
    printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
