// See mcfirmware.h. The file format follows mtlib (Modded-Cycles, from drumkilla's elektron-model-tweaks, after
// Marcel Bierling's elektron-firmware-tool; both MIT, see THIRD-PARTY-NOTICES.md):
//   SysEx:     a start message, 128-byte data messages (7-bit packed payload, masked checksum), an end message.
//   Stream:    [u32 container length][u32 container checksum] container.
//   Container: "ELE3", version at 8, section count at 0x1C, 16-byte entries {id, offset, size, load address}
//              at 0x20. Section 3 is the MAIN OS, compressed with an aPLib variant.
#include "mcfirmware.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Elektron's OS 1.13 for the Model:Cycles: SHA-256 of the firmware container and of the decompressed MAIN OS.
#define OS_VERSION "1.13"
static const char CONTAINER_SHA256[] = "015a338d5c050bd7f64afc60eb7956c7c4f28b69741e4fc9e828a9da1f7b3ea8";
static const char MAIN_OS_SHA256[] = "cc99d4f0175d34d1e91d046e6ec85a5e8ab58ab9edbb3c24406acd48cb99ee98";
#define MAIN_OS_LEN 1744192
#define FLASH_CONTAINER 0x20000

static int fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return -1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void sha256_hex(const uint8_t *p, size_t n, char hex[65]) {
    uint8_t d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(p, (CC_LONG)n, d);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
}

// MARK: SysEx transport

#define ELEKTRON_ID "\x00\x20\x3c"
#define MSG_LEN 128
#define PAYLOAD_OFF 10
#define CS_OFF 126
#define PLAIN_PER_MSG 101          // 116 packed bytes -> 101 plain bytes
#define CYCLES_ID 0x11
#define SAMPLES_ID 0x0F
#define CS_MASK_SEED 0x3A          // Model:Cycles' checksum constants
#define CS_BASE 0x22

static uint8_t msg_checksum(const uint8_t *m) {
    unsigned sum = 0;
    for (int i = 8; i < CS_OFF; i++) sum += m[i] ^ ((CS_MASK_SEED - i) & 0x3F);
    return (CS_BASE - sum) & 0x7F;
}

// 7-bit unpacking, MSB first: each group's first byte carries the high bits of the (up to) seven that follow.
static size_t unpack7(const uint8_t *p, size_t n, uint8_t *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 8)
        for (size_t k = 0; k < 7 && i + 1 + k < n; k++)
            out[o++] = (uint8_t)(((p[i] << (k + 1)) & 0x80) | p[i + 1 + k]);
    return o;
}

static uint32_t u21(const uint8_t *p) { return (uint32_t)p[0] << 14 | p[1] << 7 | p[2]; }

// Next F0 ... F7 message at or after *pos; returns its length (0 = none left).
static size_t next_msg(const uint8_t *s, size_t len, size_t *pos, const uint8_t **msg) {
    const uint8_t *a = memchr(s + *pos, 0xF0, len - *pos);
    if (!a) return 0;
    const uint8_t *b = memchr(a, 0xF7, len - (size_t)(a - s));
    if (!b) return 0;
    *msg = a;
    *pos = (size_t)(b - s) + 1;
    return (size_t)(b - a) + 1;
}

static uint8_t *unwrap(const uint8_t *s, size_t len, size_t *stream_len, char *err, size_t errlen) {
    size_t pos = 0, n;
    const uint8_t *head;
    if ((n = next_msg(s, len, &pos, &head)) < 16 || memcmp(head + 1, ELEKTRON_ID, 3)) {
        fail(err, errlen, "This is not an Elektron OS update file.");
        return NULL;
    }
    if (head[4] != CYCLES_ID) {
        fail(err, errlen, head[4] == SAMPLES_ID ? "This is the OS for the Model:Samples, not the Model:Cycles."
                                                : "This OS update is for another Elektron device (product 0x%02x).",
             head[4]);
        return NULL;
    }
    if (head[6] != 0x7F || head[7] != 0x01) {
        fail(err, errlen, "The file does not start with an OS update header.");
        return NULL;
    }
    uint32_t count = u21(head + 12);
    if (count == 0 || count > 100000) {
        fail(err, errlen, "The file's header is damaged.");
        return NULL;
    }
    uint8_t *out = malloc((size_t)count * PLAIN_PER_MSG);
    size_t o = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *m;
        if ((n = next_msg(s, len, &pos, &m)) == 0) {
            fail(err, errlen, "The file is incomplete: %u of %u data messages.", i, count);
            goto bad;
        }
        if (n != MSG_LEN || m[6] != 0x7E) {
            fail(err, errlen, "Data message %u is damaged.", i + 1);
            goto bad;
        }
        if (m[CS_OFF] != msg_checksum(m)) {
            fail(err, errlen, "Data message %u has a bad checksum: the file is damaged.", i + 1);
            goto bad;
        }
        o += unpack7(m + PAYLOAD_OFF, CS_OFF - PAYLOAD_OFF, out + o);
    }
    const uint8_t *tail;
    if ((n = next_msg(s, len, &pos, &tail)) < 8 || tail[6] != 0x7F || tail[7] != 0x02) {
        fail(err, errlen, "The file is incomplete: its end message is missing.");
        goto bad;
    }
    *stream_len = o;
    return out;
bad:
    free(out);
    return NULL;
}

// MARK: aPLib variant

typedef struct {
    const uint8_t *src;
    size_t len, ip;
    unsigned tag;
    int overrun;
} bits;

static uint8_t next_byte(bits *r) {
    if (r->ip >= r->len) { r->overrun = 1; return 0; }
    return r->src[r->ip++];
}

static int next_bit(bits *r) {
    r->tag <<= 1;
    if ((r->tag & 0xFF) == 0) {
        uint8_t b = next_byte(r);
        r->tag = (unsigned)b << 1 | 1;
        return b >> 7;
    }
    return (r->tag >> 8) & 1;
}

static uint32_t gamma2(bits *r) {                 // Elias gamma, interlaced: value bit, then "last" bit
    uint32_t v = 1;
    do {
        v = (v << 1) + (uint32_t)next_bit(r);
        if (v > 0x02000000) { r->overrun = 1; return 0; }
    } while (!next_bit(r) && !r->overrun);
    return v;
}

#define OFFSET_BIAS 767            // the end marker's offset decodes to exactly this
#define REUSE_GAMMA 2              // "same offset as the last match"
#define FAR_OFFSET 3328            // matches farther back than this are one byte longer

// sec: section bytes, starting with the [u32 stream length][u32 byte sum] header. Output at most `cap` bytes.
static uint8_t *depack(const uint8_t *sec, size_t len, size_t cap, size_t *out_len) {
    if (len < 8 || be32(sec) > len - 8) return NULL;
    uint32_t sum = 0;
    for (size_t i = 8; i < 8 + be32(sec); i++) sum += sec[i];
    if (sum != be32(sec + 4)) return NULL;
    bits r = {sec, 8 + be32(sec), 8, 0, 0};
    uint8_t *out = malloc(cap);
    size_t o = 0;
    uint32_t last = 1;
    for (;;) {
        if (next_bit(&r)) {
            if (o >= cap) goto bad;
            out[o++] = next_byte(&r);
        } else {
            uint32_t off, g = gamma2(&r);
            if (g == REUSE_GAMMA) {
                off = last;
            } else {
                off = (g << 8) + next_byte(&r);          // wraps to OFFSET_BIAS for the end marker
                if (off == OFFSET_BIAS) break;
                last = off -= OFFSET_BIAS;
            }
            uint32_t l = (uint32_t)next_bit(&r) << 1;
            l |= (uint32_t)next_bit(&r);
            if (!l) l = gamma2(&r) + 2;
            size_t n = l + 1 + (off > FAR_OFFSET);
            if (off == 0 || off > o || n > cap - o) goto bad;
            for (size_t k = 0; k < n; k++, o++) out[o] = out[o - off];   // may overlap: byte by byte
        }
        if (r.overrun) goto bad;
    }
    if (r.overrun) goto bad;
    *out_len = o;
    return out;
bad:
    free(out);
    return NULL;
}

// MARK: container

int mcf_decode(const uint8_t *syx, size_t len, mcf_firmware *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    size_t slen;
    uint8_t *stream = unwrap(syx, len, &slen, err, errlen);
    if (!stream) return -1;
    int rc = -1;
    uint32_t clen = slen >= 8 ? be32(stream) : 0;
    const uint8_t *c = stream + 8;
    if (slen < 8 + 0x20 || clen < 0x20 || clen > slen - 8 || clen > MCF_FLASH_SIZE - FLASH_CONTAINER ||
        memcmp(c, "ELE3", 4)) {
        fail(err, errlen, "The file holds no firmware container this app knows.");
        goto done;
    }
    uint32_t sum = 0;                              // sum of (word index + 1) XOR word, big-endian words
    for (uint32_t k = 0; k < clen / 4; k++) sum += (k + 1) ^ be32(c + 4 * k);
    if (sum != be32(stream + 4)) {
        fail(err, errlen, "The firmware's checksum does not match: the file is damaged.");
        goto done;
    }

    // Version: 8 characters at 8, e.g. "00381.13" (a 4-digit prefix, then the version).
    char raw[9] = {0};
    memcpy(raw, c + 8, 8);
    snprintf(out->version, sizeof out->version, "%s", strlen(raw) == 8 ? raw + 4 : raw);
    char hex[65];
    sha256_hex(c, clen, hex);
    if (strcmp(hex, CONTAINER_SHA256) != 0) {
        if (strcmp(out->version, OS_VERSION) != 0)
            fail(err, errlen, "This is OS %s. The app runs only OS " OS_VERSION " for the Model:Cycles.", out->version);
        else
            fail(err, errlen, "This is a modified OS " OS_VERSION " (for example a modded OS). The app runs only "
                              "Elektron's original OS " OS_VERSION ".");
        goto done;
    }

    uint32_t nsec = be32(c + 0x1C);
    const uint8_t *main_sec = NULL;
    uint32_t main_size = 0;
    for (uint32_t s = 0; s < nsec && 0x20 + 16 * (s + 1) <= clen; s++) {
        const uint8_t *e = c + 0x20 + 16 * s;
        if (be32(e) == 3 && be32(e + 4) <= clen && be32(e + 8) <= clen - be32(e + 4)) {
            main_sec = c + be32(e + 4);
            main_size = be32(e + 8);
        }
    }
    size_t mlen = 0;
    uint8_t *main_os = main_sec ? depack(main_sec, main_size, MAIN_OS_LEN, &mlen) : NULL;
    if (main_os) sha256_hex(main_os, mlen, hex);
    if (!main_os || mlen != MAIN_OS_LEN || strcmp(hex, MAIN_OS_SHA256) != 0) {
        free(main_os);
        fail(err, errlen, "The MAIN OS did not decompress as expected (an error in this app, not the file).");
        goto done;
    }
    out->main_os = main_os;
    out->main_os_len = mlen;
    out->flash = malloc(MCF_FLASH_SIZE);           // an erased SPI flash with the container where the OS reads it
    memset(out->flash, 0xFF, MCF_FLASH_SIZE);
    memcpy(out->flash + FLASH_CONTAINER, c, clen);
    rc = 0;
done:
    free(stream);
    return rc;
}

void mcf_free(mcf_firmware *fw) {
    free(fw->main_os);
    free(fw->flash);
    memset(fw, 0, sizeof *fw);
}

// MARK: install

static int write_file(const char *dir, const char *name, const uint8_t *p, size_t n, char *err, size_t errlen) {
    char path[1024], tmp[1100];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return fail(err, errlen, "Could not write %s: %s", path, strerror(errno));
    int ok = fwrite(p, 1, n, f) == n;
    ok = (fflush(f) == 0) && ok && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        return fail(err, errlen, "Could not write %s: %s", path, strerror(e));
    }
    return 0;
}

int mcf_install(const char *syx_path, const char *dir, char *err, size_t errlen) {
    FILE *f = fopen(syx_path, "rb");
    if (!f) return fail(err, errlen, "Could not open %s: %s", syx_path, strerror(errno));
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 64 << 20) {
        fclose(f);
        return fail(err, errlen, "This is not an Elektron OS update file.");
    }
    uint8_t *syx = malloc((size_t)st.st_size);
    size_t n = fread(syx, 1, (size_t)st.st_size, f);
    fclose(f);
    mcf_firmware fw;
    int rc = n == (size_t)st.st_size ? mcf_decode(syx, n, &fw, err, errlen)
                                     : fail(err, errlen, "Could not read %s.", syx_path);
    free(syx);
    if (rc) return rc;

    char flash[1024];
    snprintf(flash, sizeof flash, "%s/flash.bin", dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) rc = fail(err, errlen, "Could not create %s: %s", dir, strerror(errno));
    if (!rc) rc = write_file(dir, "main_os.bin", fw.main_os, fw.main_os_len, err, errlen);
    if (!rc) rc = write_file(dir, "flash.factory.bin", fw.flash, MCF_FLASH_SIZE, err, errlen);
    if (!rc && access(flash, F_OK) != 0) rc = write_file(dir, "flash.bin", fw.flash, MCF_FLASH_SIZE, err, errlen);
    mcf_free(&fw);
    return rc;
}
