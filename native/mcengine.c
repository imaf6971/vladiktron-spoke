// See mcengine.h. Addresses are OS 1.13 virtual addresses (from Modded-Cycles mcengine.py / notes/14).
#include "mcengine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

#define BASE 0x40000400u
#define VOICE_LOOP 0x400a7d4au   // (out, params, trig_mask, release_mask)
#define VOICE_RESET 0x400a7ab8u
#define TRACK_BASE 0x80001858u   // 6 x 32 x int32 voice output
#define PMOD 0x80001830u         // 6 x int32: trig note, semitones << 16
#define PARAMS 0x40700000u       // free BSS used for the 6 tracks' parameters
#define STOP 0x40780000u
#define STACK 0x90010000u
#define VOICE0 0x42308828u
#define VSTRIDE 0x31cu
#define IMAGE_LEN 0x1a9d40u      // official MAIN OS 1.13

// descriptor slot -> 8.8 word at PARAMS + 0xe + t*0x42 + 2*slot
static const int SLOT[MC_P_COUNT] = {0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, -1};
// factory defaults per machine (color, shape, sweep, contour, decay), table 0x4010dd08
static const double DEFAULTS[6][5] = {
    {10, 16, 16, 24, 28}, {0, 127, 8, 0, 40}, {46, 48, 0, 20, 20},
    {38, 100, 64, 26, 26}, {40, 38, 52, 42, 42}, {3, 43, 24, 64, 64},
};

struct mc_engine {
    uc_engine *uc;
    double trk[MC_TRACKS][MC_P_COUNT];
};

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int run(mc_engine *e, uint32_t fn, const uint32_t *args, int nargs, uint32_t sp) {
    uint8_t buf[4 * 8];
    be32(buf, STOP);
    for (int i = 0; i < nargs; i++) be32(buf + 4 + 4 * i, args[i]);
    uc_mem_write(e->uc, sp, buf, 4 + 4 * nargs);
    uc_reg_write(e->uc, UC_M68K_REG_A7, &sp);
    uc_err r = uc_emu_start(e->uc, fn, STOP, 0, 0);
    return r != UC_ERR_OK;
}

mc_engine *mc_create(const uint8_t *img, size_t len, char *err, size_t errlen) {
    if (len < IMAGE_LEN) {
        snprintf(err, errlen, "MAIN OS too short (%zu bytes)", len);
        return NULL;
    }
    mc_engine *e = calloc(1, sizeof *e);
    uc_err r = uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &e->uc);
    if (r) { snprintf(err, errlen, "uc_open: %s", uc_strerror(r)); free(e); return NULL; }
    uc_ctl_set_cpu_model(e->uc, UC_CPU_M68K_ANY);
    uc_mem_map(e->uc, 0x40000000, 0x00800000, UC_PROT_ALL);   // image + BSS/globals
    uc_mem_map(e->uc, 0x42000000, 0x00400000, UC_PROT_ALL);   // SDRAM (voice state)
    uc_mem_map(e->uc, 0x80000000, 0x00020000, UC_PROT_ALL);   // internal SRAM
    uc_mem_map(e->uc, 0x90000000, 0x00020000, UC_PROT_ALL);   // stack
    uc_mem_map(e->uc, 0xfc070000, 0x1000, UC_PROT_ALL);       // DMA timer 0 (read by the load governor)
    uc_mem_write(e->uc, BASE, img, len);
    static const uint8_t nops[4] = {0x4e, 0x71, 0x4e, 0x71};
    uc_mem_write(e->uc, STOP, nops, 4);
    // boot: ROM -> SRAM copies (0x4000045c), then voice init
    uc_mem_write(e->uc, 0x80000000, img + (0x4019b590u - BASE), 0x401a2a50u - 0x4019b590u);
    uc_mem_write(e->uc, 0x80008000, img + (0x401a2a50u - BASE), 0x401aa140u - 0x401a2a50u);
    for (int t = 0; t < MC_TRACKS; t++) {
        uint32_t v = VOICE0 + t * VSTRIDE;
        uint8_t b[4];
        be32(b, 0x8000bd98u + t * 0x30);
        uc_mem_write(e->uc, v + 0x318, b, 4);
        if (run(e, VOICE_RESET, &v, 1, STACK - 0x200)) {
            snprintf(err, errlen, "voice reset %d faulted", t);
            mc_destroy(e);
            return NULL;
        }
    }
    for (int t = 0; t < MC_TRACKS; t++) {
        double d[MC_P_COUNT] = {0, 64, 0, 0, 0, 0, 0, 0, 64, 40, 60};
        memcpy(e->trk[t], d, sizeof d);
    }
    return e;
}

void mc_destroy(mc_engine *e) {
    if (!e) return;
    uc_close(e->uc);
    free(e);
}

void mc_set(mc_engine *e, int t, mc_param p, double v) { e->trk[t][p] = v; }

void mc_machine_defaults(mc_engine *e, int t, mc_machine m) {
    const double *d = DEFAULTS[m];
    e->trk[t][MC_P_MACHINE] = m;
    e->trk[t][MC_P_COLOR] = d[0];
    e->trk[t][MC_P_SHAPE] = d[1];
    e->trk[t][MC_P_SWEEP] = d[2];
    e->trk[t][MC_P_CONTOUR] = d[3];
    e->trk[t][MC_P_DECAY] = d[4];
}

static void write_params(mc_engine *e) {
    uint8_t pm[4 * MC_TRACKS];
    for (int t = 0; t < MC_TRACKS; t++) {
        be32(pm + 4 * t, (uint32_t)(int32_t)lround(e->trk[t][MC_P_NOTE] * 65536));
        uint8_t blk[0x42];
        uint32_t p = PARAMS + 0xe + t * 0x42;
        uc_mem_read(e->uc, p, blk, sizeof blk);
        for (int k = 0; k < MC_P_COUNT; k++) {
            if (SLOT[k] < 0) continue;
            int16_t w = (int16_t)lround(e->trk[t][k] * 256);
            blk[SLOT[k] * 2] = (uint16_t)w >> 8;
            blk[SLOT[k] * 2 + 1] = (uint16_t)w;
        }
        uc_mem_write(e->uc, p, blk, sizeof blk);
    }
    uc_mem_write(e->uc, PMOD, pm, sizeof pm);
}

int mc_block(mc_engine *e, uint32_t trig, uint32_t rel, int32_t out[MC_TRACKS][MC_FRAMES]) {
    write_params(e);
    uint32_t args[4] = {TRACK_BASE, PARAMS, trig, rel};
    if (run(e, VOICE_LOOP, args, 4, STACK - 0x100)) return 1;
    uint8_t raw[MC_TRACKS * 0x80];
    uc_mem_read(e->uc, TRACK_BASE, raw, sizeof raw);
    for (int t = 0; t < MC_TRACKS; t++)
        for (int i = 0; i < MC_FRAMES; i++) {
            const uint8_t *q = raw + t * 0x80 + 4 * i;
            out[t][i] = (int32_t)((uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]);
        }
    return 0;
}
