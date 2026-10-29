"""Replace Unicorn's ColdFire EMAC with a runtime-mode model matching Modded-Cycles tools/emu/emac.py.

Accumulators hold the exact value: fractional mode in units of 2^-39 (Q62 product floored to 8 bits under the
32-bit LSB, then >> 23), integer mode in units of 1. No accumulator saturation (emac.py has none).
MACSR bits: 0x80 OMC, 0x40 unsigned, 0x20 fractional, 0x10 round. The mode is read at run time, never at
translate time, so cached code stays correct when MACSR changes.
"""
import pathlib, re, sys
root = pathlib.Path(sys.argv[1]) / 'qemu/target/m68k'

# --- helper declarations
h = root / 'helper.h'
s = h.read_text()
if 'cfmac' not in s:
    s += ('DEF_HELPER_4(cfmac, void, env, i32, i32, i32)\n'
          'DEF_HELPER_2(cfmac_get, i32, env, i32)\n'
          'DEF_HELPER_3(cfmac_set, void, env, i32, i32)\n')
    h.write_text(s)

# --- helper bodies
o = root / 'op_helper.c'
s = o.read_text()
if 'HELPER(cfmac)' not in s:
    s += r'''
/* ---- EMAC model matching Modded-Cycles emac.py (see patch_unicorn.py) ---- */
/* f: [1:0] acc, [2] sub, [3] word, [4] Rx upper, [5] Ry upper, [7:6] scale (1 = <<1, 3 = >>1),
      [8] dual, [10:9] acc2, [11] sub2 */
void HELPER(cfmac)(CPUM68KState *env, uint32_t rx, uint32_t ry, uint32_t f)
{
    int uns = (env->macsr & MACSR_SU) != 0;
    int frac = (env->macsr & MACSR_FI) != 0;
    __int128 a, b, p;
    int64_t q;
    if (f & 8) {
        uint32_t x = ((f & 0x10) ? rx >> 16 : rx) & 0xffff;
        uint32_t y = ((f & 0x20) ? ry >> 16 : ry) & 0xffff;
        a = uns ? (__int128)x : (__int128)(int16_t)x;
        b = uns ? (__int128)y : (__int128)(int16_t)y;
        p = a * b;
        if (frac) {
            p *= ((__int128)1 << 32);
        }
    } else {
        a = uns ? (__int128)rx : (__int128)(int32_t)rx;
        b = uns ? (__int128)ry : (__int128)(int32_t)ry;
        p = a * b;
    }
    switch ((f >> 6) & 3) {
    case 1: p *= 2; break;
    case 3: p >>= 1; break;
    }
    if (frac) {
        p >>= 23;
    }
    q = (int64_t)p;
    if (f & 4) {
        env->macc[f & 3] -= q;
    } else {
        env->macc[f & 3] += q;
    }
    if (f & 0x100) {
        int a2 = (f >> 9) & 3;
        if (f & 0x800) {
            env->macc[a2] -= q;
        } else {
            env->macc[a2] += q;
        }
    }
}

uint32_t HELPER(cfmac_get)(CPUM68KState *env, uint32_t acc)
{
    int64_t a = (int64_t)env->macc[acc & 3];
    int64_t v;
    if (env->macsr & MACSR_FI) {
        v = (env->macsr & MACSR_RT) ? (a + 128) >> 8 : a >> 8;
    } else {
        v = a;
    }
    if (env->macsr & MACSR_OMC) {
        if (v > 0x7fffffffLL) {
            v = 0x7fffffffLL;
        } else if (v < -0x80000000LL) {
            v = -0x80000000LL;
        }
    }
    return (uint32_t)v;
}

void HELPER(cfmac_set)(CPUM68KState *env, uint32_t val, uint32_t acc)
{
    int64_t v = (int32_t)val;
    env->macc[acc & 3] = (env->macsr & MACSR_FI) ? v * 256 : v;
}
'''
    o.write_text(s)

# --- reset: MASK starts all ones (emac.py)
c = root / 'cpu.c'
s = c.read_text()
if 'mac_mask = 0xffffffff' not in s:
    s = s.replace('    env->pc = 0;\n', '    env->pc = 0;\n    env->mac_mask = 0xffffffff;\n', 1)
    c.write_text(s)

# --- translation
t = root / 'translate.c'
s = t.read_text()
if 'gen_helper_cfmac(' not in s:
    start = s.index('DISAS_INSN(mac)\n')
    end = s.index('DISAS_INSN(from_macsr)\n')
    new_block = r'''DISAS_INSN(mac)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv rx;
    TCGv ry;
    uint16_t ext;
    int acc;
    TCGv tmp = NULL;
    TCGv addr;
    TCGv loadval;
    int dual;
    uint32_t f;

    ext = read_im16(env, s);

    acc = ((insn >> 7) & 1) | ((ext >> 3) & 2);
    /* No dual accumulate: in MAC-with-load, ext bits 3:0 are Ry, not a second accumulator (stock QEMU
       mistook them for EMAC_B dual-accumulate bits and added the product back). */
    dual = 0;
    if (insn & 0x30) {
        /* MAC with load: the mask applies only when the instruction asks for it (ext bit 5). */
        tmp = gen_lea(env, s, insn, OS_LONG);
        if (ext & 0x20) {
            addr = tcg_temp_new(tcg_ctx);
            tcg_gen_and_i32(tcg_ctx, addr, tmp, QREG_MAC_MASK);
        } else {
            addr = tmp;
        }
        loadval = gen_load(s, OS_LONG, addr, 0, IS_USER(s));
        acc ^= 1;
        rx = (ext & 0x8000) ? AREG(ext, 12) : DREG(ext, 12);
        ry = (ext & 8) ? AREG(ext, 0) : DREG(ext, 0);
    } else {
        loadval = addr = tcg_ctx->NULL_QREG;
        rx = (insn & 0x40) ? AREG(insn, 9) : DREG(insn, 9);
        ry = (insn & 8) ? AREG(insn, 0) : DREG(insn, 0);
    }

    f = acc;
    /* msac: the subtract flag is bit 8 of the extension word (stock QEMU read the opcode word). */
    if (ext & 0x100) {
        f |= 4;
    }
    if ((ext & 0x0800) == 0) {
        f |= 8;
        if (ext & 0x80) {
            f |= 0x10;
        }
        if (ext & 0x40) {
            f |= 0x20;
        }
    }
    f |= ((ext >> 9) & 3) << 6;
    if (dual) {
        f |= 0x100 | (((ext >> 2) & 3) << 9);
        if (ext & 2) {
            f |= 0x800;
        }
    }
    {
        TCGv tf = tcg_const_i32(tcg_ctx, f);
        gen_helper_cfmac(tcg_ctx, tcg_ctx->cpu_env, rx, ry, tf);
        tcg_temp_free(tcg_ctx, tf);
    }

    if (insn & 0x30) {
        TCGv rw;
        rw = (insn & 0x40) ? AREG(insn, 9) : DREG(insn, 9);
        tcg_gen_mov_i32(tcg_ctx, rw, loadval);
        /* Address writeback uses the unmasked address (emac.py). */
        switch ((insn >> 3) & 7) {
        case 3: /* Post-increment.  */
            tcg_gen_addi_i32(tcg_ctx, AREG(insn, 0), tmp, 4);
            break;
        case 4: /* Pre-decrement.  */
            tcg_gen_mov_i32(tcg_ctx, AREG(insn, 0), tmp);
        }
        tcg_temp_free(tcg_ctx, loadval);
    }
}

DISAS_INSN(from_mac)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv rx;
    int accnum;
    TCGv ta;

    rx = (insn & 8) ? AREG(insn, 0) : DREG(insn, 0);
    accnum = (insn >> 9) & 3;
    ta = tcg_const_i32(tcg_ctx, accnum);
    gen_helper_cfmac_get(tcg_ctx, rx, tcg_ctx->cpu_env, ta);
    tcg_temp_free(tcg_ctx, ta);
    if (insn & 0x40) {
        tcg_gen_movi_i64(tcg_ctx, MACREG(accnum), 0);
    }
}

DISAS_INSN(move_mac)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_mov_i64(tcg_ctx, MACREG((insn >> 9) & 3), MACREG(insn & 3));
}

'''
    s = s[:start] + new_block + s[end:]
    # to_mac
    a = s.index('DISAS_INSN(to_mac)\n'); b = s.index('DISAS_INSN(to_macsr)\n')
    s = s[:a] + r'''DISAS_INSN(to_mac)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv val;
    TCGv ta;
    int accnum;
    accnum = (insn >> 9) & 3;
    SRC_EA(env, val, OS_LONG, 0, NULL);
    ta = tcg_const_i32(tcg_ctx, accnum);
    gen_helper_cfmac_set(tcg_ctx, tcg_ctx->cpu_env, val, ta);
    tcg_temp_free(tcg_ctx, ta);
}

''' + s[b:]
    # to_macsr: plain store of the low byte (emac.py), no accumulator conversion
    a = s.index('DISAS_INSN(to_macsr)\n'); b = s.index('DISAS_INSN(to_mask)\n')
    s = s[:a] + r'''DISAS_INSN(to_macsr)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv val;
    SRC_EA(env, val, OS_LONG, 0, NULL);
    tcg_gen_andi_i32(tcg_ctx, QREG_MACSR, val, 0xff);
}

''' + s[b:]
    t.write_text(s)
print('patched')

# --- m68k exceptions: let QEMU deliver them itself (frame, VBR, RTE, trap PC), instead of stopping
ce = pathlib.Path(sys.argv[1]) / 'qemu/accel/tcg/cpu-exec.c'
s = ce.read_text()
if 'Model:Cycles board' not in s:
    anchor = '        // Unicorn: call registered interrupt callbacks\n        catched = false;\n'
    assert anchor in s
    s = s.replace(anchor, '''#if defined(TARGET_M68K)
        /* Model:Cycles board: QEMU's ColdFire exception model is complete (format word, VBR, RTE,
           trap return address), so use it rather than handing every exception to a hook. */
        {
            CPUClass *cc = CPU_GET_CLASS(cpu);
            cc->do_interrupt(cpu);
            cpu->exception_index = -1;
            return false;
        }
#endif
''' + anchor, 1)
    ce.write_text(s)

# --- m68k hardware interrupt line, for board models: uc_m68k_set_irq(uc, level, vector)
uu = root / 'unicorn.c'
s = uu.read_text()
if 'uc_m68k_set_irq' not in s:
    s += '''
/* Model:Cycles board: drive the CPU's interrupt request (level 0 = none). The CPU takes it at the next
   translation-block boundary when level > SR.I, vectoring through VBR. */
void uc_m68k_set_irq(uc_engine *uc, int level, int vector)
{
    M68kCPU *cpu = M68K_CPU(uc->cpu);
    m68k_set_irq_level(cpu, level, (uint8_t)vector);
}
'''
    uu.write_text(s)
print('patched exceptions + irq api')

# --- stop requests must not get lost: resume_all_vcpus clears cpu->exit_request, so a timeout that fires
#     before it would leave the CPU running forever. Also honour uc->stop_request in the CPU loop.
s = ce.read_text()
if 'stop_request never lost' not in s:
    anchor = '    /* Finally, check if we need to exit to the main loop.  */\n'
    assert anchor in s
    s = s.replace(anchor, '''    /* Model:Cycles board: stop_request never lost (see patch_unicorn.py). */
    if (unlikely(cpu->uc->stop_request)) {
        cpu->exit_request = 0;
        if (cpu->exception_index == -1) {
            cpu->exception_index = EXCP_INTERRUPT;
        }
        return true;
    }

''' + anchor, 1)
    ce.write_text(s)

# --- raising an interrupt must kick the CPU out of chained blocks (as upstream QEMU does)
ta = pathlib.Path(sys.argv[1]) / 'qemu/accel/tcg/tcg-all.c'
s = ta.read_text()
if 'kick the CPU' not in s:
    old = '''static void tcg_handle_interrupt(CPUState *cpu, int mask)
{
    cpu->interrupt_request |= mask;
}'''
    assert old in s
    s = s.replace(old, '''static void tcg_handle_interrupt(CPUState *cpu, int mask)
{
    cpu->interrupt_request |= mask;
    /* Model:Cycles board: kick the CPU so a running block chain notices the interrupt. */
    cpu_neg(cpu)->icount_decr.u16.high = -1;
}''')
    ta.write_text(s)
print('patched stop/irq kick')

# --- board watchdog: stop the running uc_emu_start from another thread, unconditionally
s = uu.read_text()
if 'uc_m68k_kick' not in s:
    s += '''
/* Model:Cycles board: end the current uc_emu_start at the next block boundary. Safe to call from another
   thread repeatedly; a kick that lands between runs only shortens the next one. */
void uc_m68k_kick(uc_engine *uc)
{
    uc->stop_request = true;
    if (uc->cpu) {
        cpu_exit(uc->cpu);
    }
}
'''
    uu.write_text(s)
print('patched kick')

# --- no exits after individual loads/stores on m68k: they rewind the PC to the start of an instruction
#     whose memory side effects already happened, so it would run twice (double MMIO writes, double
#     read-modify-write). Stops then happen only at block starts.
to = pathlib.Path(sys.argv[1]) / 'qemu/tcg/tcg-op.c'
s = to.read_text()
if 'never rewind a half-done instruction' not in s:
    old = '''    if (tcg_ctx->uc->no_exit_request) {
        return;
    }

    TCGv_ptr puc = tcg_const_ptr(tcg_ctx, tcg_ctx->uc);'''
    assert old in s
    s = s.replace(old, '''    if (tcg_ctx->uc->no_exit_request) {
        return;
    }
    /* Model:Cycles board: never rewind a half-done instruction on m68k (see patch_unicorn.py). */
    if (tcg_ctx->uc->arch == UC_ARCH_M68K) {
        return;
    }

    TCGv_ptr puc = tcg_const_ptr(tcg_ctx, tcg_ctx->uc);''')
    to.write_text(s)
print('patched memop exits')

# --- reading SR must not change the CPU: Unicorn's m68k reg_read sets cc_op to CC_OP_FLAGS before computing SR,
#     which turns lazily kept condition codes (cc_op says how to derive them from the saved operands) into
#     garbage, so the next conditional branch can go the wrong way (the OS jumped into nowhere when the board
#     read SR between slices). Computing the CCR from the current cc_op is all a read needs.
u = root / 'unicorn.c'
s = u.read_text()
old = '''        case UC_M68K_REG_SR:
            CHECK_REG_TYPE(uint32_t);
            env->cc_op = CC_OP_FLAGS;
            *(uint32_t *)value = cpu_m68k_get_sr(env);'''
if old in s:
    s = s.replace(old, '''        case UC_M68K_REG_SR:
            CHECK_REG_TYPE(uint32_t);
            /* Model:Cycles board: a read leaves cc_op alone (see patch_unicorn.py). */
            *(uint32_t *)value = cpu_m68k_get_sr(env);''')
    u.write_text(s)
print('patched SR read')

# --- fast stores to RAM that holds no code. Unicorn has no dirty bitmap: every RAM page reads as "clean", so
#     every TLB entry for writable RAM starts with TLB_NOTDIRTY, and notdirty_write (the slow path) only clears it
#     for pages that are not executable and have no memory hooks at all, even ones that cannot fire on RAM (our
#     unmapped-access hook). With DDR and SRAM mapped UC_PROT_ALL, every guest store took the slow path, and it
#     ran an SMC check for code on its page: about half the host time the OS cost. As upstream QEMU does:
#     a write to a page without translated code makes the entry fast, and translating code on a page
#     (tlb_protect_code) puts its write entries back on the slow path, so self-modifying code is still caught.
tr = pathlib.Path(sys.argv[1]) / 'qemu/accel/tcg/translate-all.c'
s = tr.read_text()
if 'uc_ram_page_has_code' not in s:
    s += '''
/* Model:Cycles board: does this RAM page hold translated code? (see patch_unicorn.py) */
bool uc_ram_page_has_code(struct uc_struct *uc, tb_page_addr_t addr)
{
    PageDesc *p = page_find(uc, addr >> TARGET_PAGE_BITS);
    return p && p->first_tb;
}
'''
    tr.write_text(s)
ct = pathlib.Path(sys.argv[1]) / 'qemu/accel/tcg/cputlb.c'
s = ct.read_text()
if 'uc_ram_page_has_code' not in s:
    old = '''    cpu_physical_memory_test_and_clear_dirty(ram_addr, TARGET_PAGE_SIZE,
                                             DIRTY_MEMORY_CODE);
}'''
    assert old in s
    s = s.replace(old, old[:-1] + '''    /* Model:Cycles board: writes to this page are checked for self-modifying code again. */
    if (uc->cpu) {
        tlb_reset_dirty(uc->cpu, (ram_addr_t)(uintptr_t)qemu_map_ram_ptr(uc, NULL, ram_addr & TARGET_PAGE_MASK),
                        TARGET_PAGE_SIZE);
    }
}''', 1)
    old = '''            !(tlbe->addr_code != -1) &&
            !uc_mem_hook_installed(cpu->uc, tlbe->paddr | (mem_vaddr & ~TARGET_PAGE_MASK))) {'''
    assert old in s
    s = s.replace(old, '''            !uc_ram_page_has_code(cpu->uc, ram_addr) &&
            !uc_rw_hook_installed(cpu->uc, tlbe->paddr | (mem_vaddr & ~TARGET_PAGE_MASK))) {''', 1)
    old = 'static void notdirty_write(CPUState *cpu, vaddr mem_vaddr, unsigned size,'
    s = s.replace(old, '''bool uc_ram_page_has_code(struct uc_struct *uc, tb_page_addr_t addr);
/* Model:Cycles board: hooks that need every access to this RAM address on the slow path (unmapped and
   protection hooks never fire on mapped RAM). */
static bool uc_rw_hook_installed(struct uc_struct *uc, hwaddr paddr)
{
    return HOOK_EXISTS_BOUNDED(uc, UC_HOOK_MEM_READ, paddr) ||
           HOOK_EXISTS_BOUNDED(uc, UC_HOOK_MEM_READ_AFTER, paddr) ||
           HOOK_EXISTS_BOUNDED(uc, UC_HOOK_MEM_WRITE, paddr);
}

''' + old, 1)
    ct.write_text(s)
print('patched fast RAM stores')

# --- Apple Silicon: assert_executable reads a system register behind an `isb` on every block exit, JIT write
#     toggle and hook callback (each MMIO access), only to feed an assert(); compile it out in release builds.
aj = pathlib.Path(sys.argv[1]) / 'qemu/include/tcg/tcg-apple-jit.h'
s = aj.read_text()
old = '''static inline void assert_executable(bool executable) {
    if (!pthread_jit_write_protect_supported_np()) {'''
if old in s:
    s = s.replace(old, '''static inline void assert_executable(bool executable) {
#ifdef NDEBUG   /* Model:Cycles board: release builds skip the check (see patch_unicorn.py) */
    (void)executable;
    return;
#endif
    if (!pthread_jit_write_protect_supported_np()) {''', 1)
    aj.write_text(s)
print('patched release assert')

# --- Apple Silicon: keep the JIT executable for the whole run. Unicorn made it writable after every exit from
#     generated code (cpu_tb_exec, the exit-request helper, cpu_loop_exit) and executable again before the next
#     block chain: two pthread_jit_write_protect_np calls each time the CPU leaves the JIT (interrupts, IRQ
#     kicks), the largest host cost left once the OS idles. Every write to the JIT (translation, chaining,
#     invalidation, flush) already unlocks on its own, so make it writable once, when the run ends: callers
#     outside the run see the same state as before.
q = pathlib.Path(sys.argv[1]) / 'qemu'
s = (q / 'accel/tcg/cpu-exec.c').read_text()
old = '''    ret = tcg_qemu_tb_exec(env, tb_ptr);
    if (cpu->uc->nested_level == 1) {
        // Only unlock (allow writing to JIT area) if we are the outmost uc_emu_start
        tb_exec_unlock(cpu->uc);
    }
'''
if old in s:
    s = s.replace(old, '''    ret = tcg_qemu_tb_exec(env, tb_ptr);
    /* Model:Cycles board: the JIT stays executable until the run ends (see patch_unicorn.py). */
''', 1)
    (q / 'accel/tcg/cpu-exec.c').write_text(s)
s = (q / 'accel/tcg/tcg-runtime.c').read_text()
old = '''        // from cpu_tb_exec
        if (uc->nested_level == 1) {
            // Only unlock (allow writing to JIT area) if we are the outmost uc_emu_start
            tb_exec_unlock(uc);
        }
'''
if old in s:
    s = s.replace(old, '''        /* Model:Cycles board: the JIT stays executable until the run ends (see patch_unicorn.py). */
''', 1)
    (q / 'accel/tcg/tcg-runtime.c').write_text(s)
s = (q / 'accel/tcg/cpu-exec-common.c').read_text()
old = '''    /* Unlock JIT write protect if applicable. */
    if (cpu->uc->nested_level == 1) {
        tb_exec_unlock(cpu->uc);
    }
'''
if old in s:
    s = s.replace(old, '''    /* Model:Cycles board: the JIT stays executable until the run ends (see patch_unicorn.py). */
''', 1)
    (q / 'accel/tcg/cpu-exec-common.c').write_text(s)
s = (q / 'softmmu/cpus.c').read_text()
old = '''        if (tcg_cpu_exec(uc)) {
            break;
        }
    }
'''
if old in s and 'JIT writable again' not in s:
    s = s.replace(old, old + '''    /* Model:Cycles board: the run is over: JIT writable again, as Unicorn leaves it (see patch_unicorn.py). */
    if (uc->nested_level == 1) {
        tb_exec_unlock(uc);
    }
''', 1)
    (q / 'softmmu/cpus.c').write_text(s)
for f, mark in (('accel/tcg/cpu-exec.c', 'stays executable'), ('accel/tcg/tcg-runtime.c', 'stays executable'),
                ('accel/tcg/cpu-exec-common.c', 'stays executable'), ('softmmu/cpus.c', 'JIT writable again')):
    assert mark in (q / f).read_text(), f
print('patched JIT write toggles')

# --- instruction budget: the board keeps its own time (CLOCK-PLAN.md), counted in guest instructions. Unicorn's
#     count option installs a code hook on every instruction; QEMU's icount decrement cannot be turned on, since
#     Unicorn calls a helper at every block start instead (an inline branch there breaks TCG's liveness analysis,
#     gen-icount.h). That helper now also counts: its second argument, a constant 0 on m68k (no delay slots), is
#     patched at the block's end to (instructions << 1). A block is charged when it starts, in full; once the count
#     reaches the limit the run stops at the next block start, before charging it. One add and one compare per
#     block, on a call that was there already.
s = (q / 'include/tcg/tcg.h').read_text()
if 'board_insn_op' not in s:
    old = '    TCGOp *icount_start_insn;\n'
    assert old in s
    s = s.replace(old, old + '    TCGOp *board_insn_op;   /* Model:Cycles board: the block\'s instruction count (patch_unicorn.py) */\n', 1)
    (q / 'include/tcg/tcg.h').write_text(s)
s = (q / 'include/exec/gen-icount.h').read_text()
if 'board_insn_op' not in s:
    old = '''    if (tcg_ctx->delay_slot_flag != NULL) {
        tcg_gen_mov_i32(tcg_ctx, tmp, tcg_ctx->delay_slot_flag);
    }
    gen_helper_check_exit_request(tcg_ctx, puc, tmp);'''
    assert old in s
    s = s.replace(old, '''    /* Model:Cycles board: without delay slots the argument carries the block's instruction count, filled in by
       gen_tb_end (see patch_unicorn.py). */
    tcg_ctx->board_insn_op = NULL;
    if (tcg_ctx->delay_slot_flag != NULL) {
        tcg_gen_mov_i32(tcg_ctx, tmp, tcg_ctx->delay_slot_flag);
    } else {
        tcg_ctx->board_insn_op = tcg_last_op(tcg_ctx);
    }
    gen_helper_check_exit_request(tcg_ctx, puc, tmp);''', 1)
    old = '''    tcg_ctx->delay_slot_flag = NULL;
    if (tb_cflags(tb) & CF_USE_ICOUNT) {'''
    assert old in s
    s = s.replace(old, '''    tcg_ctx->delay_slot_flag = NULL;
    if (tcg_ctx->board_insn_op != NULL) {
        tcg_set_insn_param(tcg_ctx->board_insn_op, 1, (TCGArg)num_insns << 1);
        tcg_ctx->board_insn_op = NULL;
    }
    if (tb_cflags(tb) & CF_USE_ICOUNT) {''', 1)
    (q / 'include/exec/gen-icount.h').write_text(s)
up = pathlib.Path(sys.argv[1]) / 'include/uc_priv.h'
s = up.read_text()
if 'board_icount' not in s:
    old = '    int nested_level;                         // Current nested_level\n'
    assert old in s
    s = s.replace(old, old + '''    uint64_t board_icount, board_ilimit; // Model:Cycles board: instructions run, and where the run stops
                                         // (patch_unicorn.py)
''', 1)
    up.write_text(s)
s = (q / 'accel/tcg/tcg-runtime.c').read_text()
if 'board_icount' not in s:
    old = '''    uc_engine *uc = p;

    if (cpu_loop_exit_requested(uc->cpu) && !in_delay_slot) {'''
    assert old in s
    s = s.replace(old, '''    uc_engine *uc = p;
    /* Model:Cycles board: instruction budget (see patch_unicorn.py). Bit 0 is the delay-slot flag of the
       architectures that have one; the rest is the block's instruction count. */
    uint32_t insns = in_delay_slot >> 1;
    in_delay_slot &= 1;
    if (uc->board_icount >= uc->board_ilimit && !in_delay_slot) {
        uc->stop_request = true;
        cpu_exit(uc->cpu);
    }

    if (cpu_loop_exit_requested(uc->cpu) && !in_delay_slot) {''', 1)
    old = '''            cpu_loop_exit_restore(uc->cpu, GETPC());
        }
    }
}'''
    assert old in s
    s = s.replace(old, '''            cpu_loop_exit_restore(uc->cpu, GETPC());
        }
    }
    uc->board_icount += insns;
}''', 1)
    (q / 'accel/tcg/tcg-runtime.c').write_text(s)
s = uu.read_text()
if 'uc_m68k_icount' not in s:
    s += '''
/* Model:Cycles board: guest instructions run so far (charged a block at a time, when the block starts), and the
   count at which the run stops (at the next block start; UINT64_MAX: never). Call from the CPU thread. */
uint64_t uc_m68k_icount(uc_engine *uc)
{
    return uc->board_icount;
}

void uc_m68k_set_ilimit(uc_engine *uc, uint64_t limit)
{
    uc->board_ilimit = limit;
}
'''
    uu.write_text(s)
s = (pathlib.Path(sys.argv[1]) / 'uc.c').read_text()
if 'board_ilimit = UINT64_MAX' not in s:
    old = '        uc->errnum = UC_ERR_OK;\n'
    assert old in s
    s = s.replace(old, old + '        uc->board_ilimit = UINT64_MAX;   /* Model:Cycles board: no budget (patch_unicorn.py) */\n', 1)
    (pathlib.Path(sys.argv[1]) / 'uc.c').write_text(s)
print('patched instruction budget')
