// Checks of the OS file decoder (mcfirmware.c) with files made up here: each way a file can be wrong gets its own
// message, and a good transport around a container that is not OS 1.13 is refused by version. No Elektron data is
// needed, so this runs anywhere (ok/FAIL). With the real file next to the repository (../model-cycles_OS1.13.syx, or
// the path given), it also checks that it installs.
//   mcfirmware_test [model-cycles_OS1.13.syx]
// It includes mcfirmware.c to reach its checksum and packing rules.
#include "mcfirmware.c"

static int failures;

static void check(int ok, const char *what, const char *got) {
    printf("%s  %-58s %s\n", ok ? "ok  " : "FAIL", what, ok ? "" : got);
    if (!ok) failures++;
}

// ---- building files: SysEx messages as the Model:Cycles' OS update has them

typedef struct { uint8_t *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put_u21(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 14 & 0x7F); p[1] = (uint8_t)(v >> 7 & 0x7F); p[2] = (uint8_t)(v & 0x7F); }
static void put_be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void start_msg(buf *b, uint8_t product, uint32_t count) {
    uint8_t m[16] = {0xF0, 0x00, 0x20, 0x3C, product, 0x00, 0x7F, 0x01};
    put_u21(m + 12, count);
    m[15] = 0xF7;
    put(b, m, sizeof m);
}
static void end_msg(buf *b) {
    static const uint8_t m[9] = {0xF0, 0x00, 0x20, 0x3C, CYCLES_ID, 0x00, 0x7F, 0x02, 0xF7};
    put(b, m, sizeof m);
}
// One 128-byte data message carrying 101 plain bytes (7-bit packed, as unpack7 reads them back).
static void data_msg(buf *b, const uint8_t plain[PLAIN_PER_MSG], int bad_checksum) {
    uint8_t m[MSG_LEN] = {0xF0, 0x00, 0x20, 0x3C, CYCLES_ID, 0x00, 0x7E};
    uint8_t *o = m + PAYLOAD_OFF;
    for (int i = 0; i < PLAIN_PER_MSG; i += 7) {
        uint8_t *hdr = o++;
        *hdr = 0;
        for (int k = 0; k < 7 && i + k < PLAIN_PER_MSG; k++) {
            *hdr |= (uint8_t)((plain[i + k] >> 7) << (6 - k));
            *o++ = plain[i + k] & 0x7F;
        }
    }
    m[CS_OFF] = (uint8_t)(msg_checksum(m) ^ (bad_checksum ? 1 : 0));
    m[MSG_LEN - 1] = 0xF7;
    put(b, m, sizeof m);
}
// A whole file around `stream` (the [length][checksum] header and container).
static buf file_of(const uint8_t *stream, size_t n) {
    buf b = {0};
    uint32_t count = (uint32_t)((n + PLAIN_PER_MSG - 1) / PLAIN_PER_MSG);
    start_msg(&b, CYCLES_ID, count);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t plain[PLAIN_PER_MSG] = {0};
        size_t k = n - i * PLAIN_PER_MSG < PLAIN_PER_MSG ? n - i * PLAIN_PER_MSG : PLAIN_PER_MSG;
        memcpy(plain, stream + i * PLAIN_PER_MSG, k);
        data_msg(&b, plain, 0);
    }
    end_msg(&b);
    return b;
}
// A stream holding an "ELE3" container with version `version` (8 characters), its checksum right.
static size_t container_stream(uint8_t *s, const char *version) {
    uint32_t clen = 0x40;
    uint8_t *c = s + 8;
    memset(s, 0, 8 + clen);
    memcpy(c, "ELE3", 4);
    memcpy(c + 8, version, 8);
    uint32_t sum = 0;
    for (uint32_t k = 0; k < clen / 4; k++) sum += (k + 1) ^ be32(c + 4 * k);
    put_be(s, clen);
    put_be(s + 4, sum);
    return 8 + clen;
}

static void expect(const uint8_t *syx, size_t n, const char *want, const char *what) {
    mcf_firmware fw;
    char err[256] = "";
    int rc = mcf_decode(syx, n, &fw, err, sizeof err);
    check(rc != 0 && strstr(err, want) != NULL, what, err);
    if (rc == 0) mcf_free(&fw);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint8_t plain[PLAIN_PER_MSG];
    for (int i = 0; i < PLAIN_PER_MSG; i++) plain[i] = (uint8_t)(i * 7);

    static const uint8_t junk[] = "PK\x03\x04 not a sysex file";
    expect(junk, sizeof junk, "not an Elektron OS update", "a file that is not SysEx");
    {
        static const uint8_t other[] = {0xF0, 0x43, 0x10, 0x4C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7};   // Yamaha
        expect(other, sizeof other, "not an Elektron OS update", "SysEx from another maker");
    }
    buf b = {0};
    start_msg(&b, SAMPLES_ID, 1); data_msg(&b, plain, 0); end_msg(&b);
    expect(b.p, b.n, "Model:Samples", "the Model:Samples OS");
    b.n = 0; start_msg(&b, 0x22, 1);
    expect(b.p, b.n, "another Elektron device (product 0x22)", "another Elektron device's OS");
    b.n = 0; start_msg(&b, CYCLES_ID, 1); b.p[7] = 0x05;
    expect(b.p, b.n, "does not start with an OS update header", "a Model:Cycles message that is not an OS update");
    b.n = 0; start_msg(&b, CYCLES_ID, 0);
    expect(b.p, b.n, "header is damaged", "a header that announces no data");
    b.n = 0; start_msg(&b, CYCLES_ID, 2); data_msg(&b, plain, 0);                    // the rest is cut off
    expect(b.p, b.n, "1 of 2 data messages", "a file cut short (1 of 2 data messages)");
    end_msg(&b);
    expect(b.p, b.n, "Data message 2 is damaged", "a file with a data message missing");
    b.n = 0; start_msg(&b, CYCLES_ID, 1); data_msg(&b, plain, 1); end_msg(&b);
    expect(b.p, b.n, "bad checksum", "a data message with a bad checksum");
    b.n = 0; start_msg(&b, CYCLES_ID, 1); data_msg(&b, plain, 0); b.n -= 2; put(&b, "\xF7", 1);
    expect(b.p, b.n, "Data message 1 is damaged", "a data message of the wrong length");
    b.n = 0; start_msg(&b, CYCLES_ID, 1); data_msg(&b, plain, 0);
    expect(b.p, b.n, "end message is missing", "a file without its end message");

    buf f = file_of(plain, sizeof plain);                     // a good transport, but no container
    expect(f.p, f.n, "no firmware container", "a good file around something that is not a container");
    free(f.p);
    uint8_t s[8 + 0x40];
    size_t sn = container_stream(s, "00381.12");
    f = file_of(s, sn);
    expect(f.p, f.n, "This is OS 1.12", "another OS version is named");
    free(f.p);
    sn = container_stream(s, "00381.13");
    f = file_of(s, sn);
    expect(f.p, f.n, "modified OS 1.13", "a 1.13 container that is not Elektron's");
    free(f.p);
    s[8 + 0x30] ^= 1;                                         // one bit off: the container checksum fails
    f = file_of(s, sn);
    expect(f.p, f.n, "checksum does not match", "a container with a bad checksum");
    free(f.p);
    free(b.p);

    char err[256] = "";
    check(mcf_install("/nonexistent/model-cycles_OS1.13.syx", "/tmp", err, sizeof err) != 0 && strstr(err, "Could not open"),
          "install: a file that is not there", err);

    const char *real = argc > 1 ? argv[1] : "../model-cycles_OS1.13.syx";
    FILE *rf = fopen(real, "rb");
    if (rf) {
        fseek(rf, 0, SEEK_END); size_t n = (size_t)ftell(rf); fseek(rf, 0, SEEK_SET);
        uint8_t *syx = malloc(n);
        size_t got = fread(syx, 1, n, rf);
        fclose(rf);
        mcf_firmware fw;
        int rc = got == n ? mcf_decode(syx, n, &fw, err, sizeof err) : -1;
        check(rc == 0 && fw.main_os_len == MAIN_OS_LEN && strcmp(fw.version, "1.13") == 0, "the real OS 1.13 file decodes", err);
        if (rc == 0) mcf_free(&fw);
        expect(syx, n / 2, "incomplete", "the real file cut in half");
        free(syx);
    } else {
        printf("      (no %s: the real file is not checked)\n", real);
    }
    printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
