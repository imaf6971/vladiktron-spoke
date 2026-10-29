// Checks the ColdFire EMAC multiply-accumulate that patch_unicorn.py translates into TCG ops (gen_cfmac_inline)
// against the helper it replaced (HELPER(cfmac), copied below as the reference): random MAC instructions, register
// and load forms, every operand half, scale, add/subtract and MACSR mode, run through the JIT. Each case also
// changes MACSR between two MACs in one block, which must end the block (the mode is part of the block's key).
// No firmware needed.
//   mcemac_test [cases]
// Prints ok/FAIL; exit status 0 only if all pass.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

int64_t uc_m68k_macc(uc_engine *uc, int acc);                  // patched Unicorn (patch_unicorn.py)
void uc_m68k_set_macc(uc_engine *uc, int acc, int64_t value);

#define CODE 0x100000u
#define DATA 0x200000u
#define MACSR_SU 0x40
#define MACSR_FI 0x20

// HELPER(cfmac) as patch_unicorn.py writes it (op_helper.c), on plain values.
static void ref_cfmac(int64_t macc[4], uint32_t macsr, uint32_t rx, uint32_t ry, uint32_t f) {
    int uns = (macsr & MACSR_SU) != 0;
    int frac = (macsr & MACSR_FI) != 0;
    __int128 a, b, p;
    int64_t q;
    if (f & 8) {
        uint32_t x = ((f & 0x10) ? rx >> 16 : rx) & 0xffff;
        uint32_t y = ((f & 0x20) ? ry >> 16 : ry) & 0xffff;
        a = uns ? (__int128)x : (__int128)(int16_t)x;
        b = uns ? (__int128)y : (__int128)(int16_t)y;
        p = a * b;
        if (frac) p *= ((__int128)1 << 32);
    } else {
        a = uns ? (__int128)rx : (__int128)(int32_t)rx;
        b = uns ? (__int128)ry : (__int128)(int32_t)ry;
        p = a * b;
    }
    switch ((f >> 6) & 3) {
    case 1: p *= 2; break;
    case 3: p >>= 1; break;
    }
    if (frac) p >>= 23;
    q = (int64_t)p;
    if (f & 4) macc[f & 3] -= q; else macc[f & 3] += q;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint32_t rnd_val(void) {                                  // edge values often: they find the overflow cases
    static const uint32_t edge[] = {0, 1, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000u, 0xffffffffu,
                                    0x80008000u, 0x7fff7fff, 0xffff0001u};
    return rnd() % 4 == 0 ? edge[rnd() % (sizeof edge / sizeof *edge)] : rnd() << 16 ^ rnd();
}

typedef struct { uint32_t d[8], a[8]; int64_t macc[4]; } state;

// One MAC instruction (2 words) into `code`, and what it does to `s` (the model of the translator's decoding).
static void gen_mac(uint16_t *code, state *s, uint32_t macsr, uint32_t mem[4]) {
    uint16_t insn = 0xA000 | (uint16_t)(rnd() & 0x0EC0) | (uint16_t)(rnd() & 0x0F), ext = (uint16_t)rnd();
    int load = rnd() % 3 == 0, mode = 0;
    if (load) {                                                  // (A0), (A0)+ or -(A0)
        mode = 2 + (int)(rnd() % 3);
        insn = (uint16_t)((insn & ~0x3F) | mode << 3);
        if ((insn & 0x40) && ((insn >> 9) & 7) == 0) insn |= 1 << 9;   // (loading into A0 would move the next load)
        ext &= ~0x20;                                            // (no MAC_MASK)
    }
    code[0] = __builtin_bswap16(insn);
    code[1] = __builtin_bswap16(ext);
    int acc = ((insn >> 7) & 1) | ((ext >> 3) & 2);
    uint32_t rx, ry;
    if (load) {
        acc ^= 1;
        rx = (ext & 0x8000) ? s->a[(ext >> 12) & 7] : s->d[(ext >> 12) & 7];
        ry = (ext & 8) ? s->a[ext & 7] : s->d[ext & 7];
    } else {
        rx = (insn & 0x40) ? s->a[(insn >> 9) & 7] : s->d[(insn >> 9) & 7];
        ry = (insn & 8) ? s->a[insn & 7] : s->d[insn & 7];
    }
    uint32_t f = (uint32_t)acc;
    if (ext & 0x100) f |= 4;
    if ((ext & 0x0800) == 0) {
        f |= 8;
        if (ext & 0x80) f |= 0x10;
        if (ext & 0x40) f |= 0x20;
    }
    f |= ((ext >> 9) & 3u) << 6;
    ref_cfmac(s->macc, macsr, rx, ry, f);
    if (load) {
        uint32_t addr = mode == 4 ? s->a[0] - 4 : s->a[0];
        uint32_t v = mem[(addr - DATA) / 4 % 4];
        if (insn & 0x40) s->a[(insn >> 9) & 7] = v; else s->d[(insn >> 9) & 7] = v;
        if (mode == 3) s->a[0] = addr + 4;
        if (mode == 4) s->a[0] = addr;
    }
}

int main(int argc, char **argv) {
    int cases = argc > 1 ? atoi(argv[1]) : 200000, failures = 0;
    uc_engine *uc;
    if (uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &uc) != UC_ERR_OK) return 2;
    uc_ctl_set_cpu_model(uc, UC_CPU_M68K_ANY);
    uc_mem_map(uc, CODE, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc, DATA, 0x1000, UC_PROT_ALL);
    for (int c = 0; c < cases; c++) {
        state s, got;
        uint32_t mem[4], macsr[2] = {rnd() & 0xF0, rnd() & 0xF0};   // (mode bits; flags left clear)
        uint8_t raw[16];
        for (int i = 0; i < 8; i++) { s.d[i] = rnd_val(); s.a[i] = rnd_val(); }
        for (int i = 0; i < 4; i++) { mem[i] = rnd_val(); s.macc[i] = (int64_t)((uint64_t)rnd_val() << 32 | rnd_val()); }
        s.d[7] = macsr[0]; s.d[6] = macsr[1];
        s.a[0] = DATA + 8;                                       // the load forms' address register
        for (int i = 0; i < 4; i++) { raw[4*i] = (uint8_t)(mem[i] >> 24); raw[4*i+1] = (uint8_t)(mem[i] >> 16); raw[4*i+2] = (uint8_t)(mem[i] >> 8); raw[4*i+3] = (uint8_t)mem[i]; }
        uc_mem_write(uc, DATA, raw, 16);
        uc_mem_write(uc, DATA + 16, raw, 16);                    // (an (A0)+ or a second load lands here or before)
        for (int i = 0; i < 8; i++) {
            uc_reg_write(uc, UC_M68K_REG_D0 + i, &s.d[i]);
            uc_reg_write(uc, UC_M68K_REG_A0 + i, &s.a[i]);
        }
        for (int i = 0; i < 4; i++) uc_m68k_set_macc(uc, i, s.macc[i]);
        // move.l d7,macsr; mac; move.l d6,macsr; mac. The MACs may overwrite d6/d7: MACSR takes them first
        // (d6 is read by the second move, after the first MAC: keep the model in step with that).
        uint16_t code[8];
        code[0] = __builtin_bswap16(0xA907);
        gen_mac(code + 1, &s, macsr[0], mem);
        uint32_t second = s.d[6] & 0xFF;
        code[3] = __builtin_bswap16(0xA906);
        gen_mac(code + 4, &s, second, mem);
        uc_mem_write(uc, CODE, code, 12);
        uc_ctl_remove_cache(uc, CODE, CODE + 12);                // (uc_mem_write leaves old translations in place)
        uc_err e = uc_emu_start(uc, CODE, CODE + 12, 0, 0);
        for (int i = 0; i < 8; i++) {
            uc_reg_read(uc, UC_M68K_REG_D0 + i, &got.d[i]);
            uc_reg_read(uc, UC_M68K_REG_A0 + i, &got.a[i]);
        }
        for (int i = 0; i < 4; i++) got.macc[i] = uc_m68k_macc(uc, i);
        if (e != UC_ERR_OK || memcmp(got.d, s.d, sizeof s.d) || memcmp(got.a, s.a, sizeof s.a) ||
            memcmp(got.macc, s.macc, sizeof s.macc)) {
            if (failures++ < 5) {
                printf("FAIL  case %d (%s): code", c, uc_strerror(e));
                for (int i = 0; i < 6; i++) printf(" %04x", __builtin_bswap16(code[i]));
                printf(", macsr %02x then %02x\n", macsr[0], second);
                for (int i = 0; i < 4; i++)
                    if (got.macc[i] != s.macc[i]) printf("      acc%d %016" PRIx64 ", want %016" PRIx64 "\n", i, (uint64_t)got.macc[i], (uint64_t)s.macc[i]);
                for (int i = 0; i < 8; i++) {
                    if (got.d[i] != s.d[i]) printf("      d%d %08x, want %08x\n", i, got.d[i], s.d[i]);
                    if (got.a[i] != s.a[i]) printf("      a%d %08x, want %08x\n", i, got.a[i], s.a[i]);
                }
            }
        }
    }
    printf("%s  %d random MAC pairs give the reference helper's accumulators and registers (%d differ)\n",
           failures ? "FAIL" : "ok  ", cases, failures);
    uc_close(uc);
    return failures ? 1 : 0;
}
