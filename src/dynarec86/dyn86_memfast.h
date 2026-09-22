/* src/dynarec86/dyn86_memfast.h — D2Vita: the INLINE SHORT PATH of the CRT
 * memcpy/memset intrinsics (D2_MEMINTRIN=3). Contract and rationale:
 * dyn86_memintrin.h. Box86 is (c) ptitSeb, MIT — third_party/box86-dynarec.
 * ---------------------------------------------------------------------------
 * WHY THIS FILE IS A MACRO. The emission must happen inside
 * dynarec_arm_pass.c, the only place where EMIT() (pass-specific: pass2 counts
 * bytes, pass3 writes them) is defined. Keeping the sequence here instead of
 * inline in that file keeps the translator's diff to three lines, which
 * matters because that file is shared with the generic intrinsics table.
 *
 * WHERE IT SITS. Right at the start of the block that BEGINS at the memcpy /
 * memset entry point, BEFORE the mode-1 helper-call sequence:
 *
 *     <inline acceptance tests>   -- any refusal branches to memfast_mark
 *     <inline copy>               -- no call, no trap
 *     <ret_to_epilog>             -- the normal dynarec RET
 *   memfast_mark:
 *     <mode-1 helper call>        -- unchanged; serves what the inline path refused
 *   memintrin_mark:
 *     <original translated body>  -- faithful fallback, byte for byte
 *
 * memfast_mark uses the same mark mechanism as memintrin_mark and budget_tail:
 * pass2 measures it, pass3 reads it. In pass2 the value is stale, so the
 * computed offset is wrong -- with no consequence, since Bcond is ONE
 * instruction whatever the offset and size is all pass2 produces (a pass2 /
 * pass3 size divergence is detected and cancels the block, dynarec_arm.c).
 *
 * REGISTERS USED. x1/x2/x3 (the dynarec's scratch), xEIP (stale at block
 * entry -- the block prologue says so, and ret_to_epilog rewrites it before
 * anyone reads it), q0/q1 for memcpy (the NEON cache is empty at block entry:
 * fpu_pushcache emits nothing there, and d0-d15 are caller-saved by the ARM
 * ABI so no live guest value can sit in them across a block boundary), and
 * xEAX -- written ONLY past the last refusal branch, so a faithful fallback
 * never touches EAX.
 *
 * FIDELITY OF THE COPY ITSELF.
 *  - size: the "by bits" cascade copies exactly n bytes, n < 64: 32 if bit 5
 *    is set, then 16, 8, 4, 2 (as two bytes), 1. Never one byte more, which is
 *    what the D2_MEMFASTCHECK guards verify at runtime.
 *  - direction: ascending. Legal because overlap is REFUSED (below), so
 *    source and destination are disjoint.
 *  - overlap test: the regions [d,d+n) and [s,s+n) overlap iff |d-s| < n. The
 *    emitted form is the unsigned test (uint32)(d-s+n) < 2n, which flags every
 *    real overlap and additionally flags the exactly-adjacent case d-s == -n
 *    -- a refusal, i.e. conservative in the safe direction.
 *  - arena: dst (and src) must be <= span - 64, so dst+n stays inside the
 *    guest arena for any n < 64. Same bound as the helper's, rounded down by
 *    at most 63 bytes, again a refusal in the safe direction.
 *  - alignment: none required. Every access is byte-element (VLD1.8 / LDR /
 *    LDRB); unaligned LDR/STR is what the whole dynarec already emits for
 *    guest accesses, and VLD1.8/VST1.8 are byte-element accesses.
 *
 * HOW TO RE-READ THE EMITTED CODE. Build with EXTRA=-DD2_MIDUMP LIBTAG=_midump
 * (dyn86_memintrin.c): on the first helper call the return address points
 * INTO the block, just past the BLX, and the words around it are printed as
 * [midump] lines. Reassemble them with `.inst` and disassemble -- that is how
 * the sequence in the report of 2026-09-22 was obtained, and it is the only
 * proof that counts here: the macro below is not the code, the block is.
 */
#ifndef DYN86_MEMFAST_H_
#define DYN86_MEMFAST_H_

/* ---- encodings not present in arm_emitter.h (checked against
 * arm-linux-gnueabihf-as, see the report of 2026-09-21) -------------------- */
/* ldr<c>  Rt, [Rn], #imm   / str<c> / ldrb<c> / strb<c>, post-indexed, U=1 */
#define D2MF_LDR_POST(cond, reg, addr, imm)  EMIT((cond) | 0x04900000 | ((addr)<<16) | ((reg)<<12) | (imm))
#define D2MF_STR_POST(cond, reg, addr, imm)  EMIT((cond) | 0x04800000 | ((addr)<<16) | ((reg)<<12) | (imm))
#define D2MF_LDRB_POST(cond, reg, addr, imm) EMIT((cond) | 0x04D00000 | ((addr)<<16) | ((reg)<<12) | (imm))
#define D2MF_STRB_POST(cond, reg, addr, imm) EMIT((cond) | 0x04C00000 | ((addr)<<16) | ((reg)<<12) | (imm))
/* vld1.8/vst1.8 {Dd-Dd+1}, [Rn]!  (16 bytes) and {Dd}, [Rn]!  (8 bytes) */
#define D2MF_VLD1Q_8_W(Dd, Rn) EMIT(Vxx1gen(1, ((Dd)>>4)&1, Rn, ((Dd)&0x0f), 0b1010, 0, 0, 13))
#define D2MF_VST1Q_8_W(Dd, Rn) EMIT(Vxx1gen(0, ((Dd)>>4)&1, Rn, ((Dd)&0x0f), 0b1010, 0, 0, 13))
#define D2MF_VLD1_8_W(Dd, Rn)  EMIT(Vxx1gen(1, ((Dd)>>4)&1, Rn, ((Dd)&0x0f), 0b0111, 0, 0, 13))
#define D2MF_VST1_8_W(Dd, Rn)  EMIT(Vxx1gen(0, ((Dd)>>4)&1, Rn, ((Dd)&0x0f), 0b0111, 0, 0, 13))

/* Branch to memfast_mark (the helper-call sequence emitted just after us). */
#define D2MF_BAIL(cond) do { j32 = dyn->memfast_mark - (dyn->arm_size + 8); Bcond((cond), j32); } while(0)
/* Skip the next K instructions when the tested bit is clear. */
#define D2MF_SKIP(k)    Bcond(cEQ, 4*(k) - 4)

/* Guest arena bound used by the emitted test: dst <= LIM guarantees
 * dst + n <= span for every n < DYN86_MI_FASTN. span == 0 (no arena, the
 * qemu identity path) means "no bound", exactly like the helper. */
#define D2MF_LIM ((uint32_t)((dyn86_mi_span ? dyn86_mi_span : 0xffffffffu) - DYN86_MI_FASTN))

/* ---- the sequence ---------------------------------------------------------
 * `is_set` selects memset (dst, c, n) over memcpy (dst, src, n).
 * Uses `dyn`, `ninst` and `j32` from the enclosing scope. */
#define D2MF_EMIT(is_set)                                                      \
do {                                                                           \
    const uint8_t mf_mb = (uint8_t)(dyn86_membase >> 24);                      \
    /* r1 = HOST pointer to the guest stack frame */                           \
    if(dyn86_membase) { ADD_IMM8_ROR(x1, xESP, mf_mb, 4); }                    \
    else              { MOV_REG(x1, xESP); }                                   \
    LDR_IMM9(x2, x1, 12);                  /* r2 = n                        */ \
    CMPS_IMM8(x2, DYN86_MI_FASTN);                                             \
    D2MF_BAIL(cCS);                        /* n >= 64 -> helper             */ \
    LDR_IMM9(x3, x1, 4);                   /* r3 = dst (guest)              */ \
    LDR_IMM9(x1, x1, 8);                   /* r1 = src (guest) / fill value */ \
    MOV32_(x14, D2MF_LIM);                 /* 2 insns, fixed size           */ \
    CMPS_REG_LSL_IMM5(x3, x14, 0);                                             \
    if(!(is_set)) { CMPS_REG_LSL_IMM5_COND(cLS, x1, x14, 0); }                 \
    D2MF_BAIL(cHI);                        /* outside the arena -> helper   */ \
    if(!(is_set)) {                                                            \
        SUB_REG_LSL_IMM5(x14, x3, x1, 0);  /* dst - src                     */ \
        ADD_REG_LSL_IMM5(x14, x14, x2, 0); /* + n                           */ \
        CMPS_REG_LSL_IMM5(x14, x2, 1);     /* vs 2n                         */ \
        D2MF_BAIL(cCC);                    /* overlap -> helper (memmove)   */ \
    }                                                                          \
    /* ---- committed: from here the call IS served ---- */                    \
    if(dyn86_mi_fastchk) {                 /* oracle PRE (check build only) */ \
        MOV_REG(x1, xESP);                                                     \
        CALL_((is_set)?(void*)dyn86_mi_fast_pre_set:(void*)dyn86_mi_fast_pre_cpy, -1, 0); \
        if(dyn86_membase) { ADD_IMM8_ROR(x1, xESP, mf_mb, 4); }                \
        else              { MOV_REG(x1, xESP); }                               \
        LDR_IMM9(x2, x1, 12);                                                  \
        LDR_IMM9(x3, x1, 4);                                                   \
        LDR_IMM9(x1, x1, 8);                                                   \
    }                                                                          \
    MOV_REG(xEAX, x3);                     /* memcpy/memset return dst      */ \
    if(dyn86_membase) { ADD_IMM8_ROR(x3, x3, mf_mb, 4); }                      \
    if(is_set) {                                                               \
        AND_IMM8(x14, x1, 0xff);                                               \
        ORR_REG_LSL_IMM5(x14, x14, x14, 8);                                    \
        ORR_REG_LSL_IMM5(x14, x14, x14, 16);  /* r14 = the byte, splatted   */ \
        TSTS_IMM8(x2, 32); D2MF_SKIP(8);                                       \
        D2MF_STR_POST(c__, x14, x3, 4); D2MF_STR_POST(c__, x14, x3, 4);        \
        D2MF_STR_POST(c__, x14, x3, 4); D2MF_STR_POST(c__, x14, x3, 4);        \
        D2MF_STR_POST(c__, x14, x3, 4); D2MF_STR_POST(c__, x14, x3, 4);        \
        D2MF_STR_POST(c__, x14, x3, 4); D2MF_STR_POST(c__, x14, x3, 4);        \
        TSTS_IMM8(x2, 16); D2MF_SKIP(4);                                       \
        D2MF_STR_POST(c__, x14, x3, 4); D2MF_STR_POST(c__, x14, x3, 4);        \
        D2MF_STR_POST(c__, x14, x3, 4); D2MF_STR_POST(c__, x14, x3, 4);        \
        TSTS_IMM8(x2, 8); D2MF_SKIP(2);                                        \
        D2MF_STR_POST(c__, x14, x3, 4); D2MF_STR_POST(c__, x14, x3, 4);        \
        TSTS_IMM8(x2, 4);                                                      \
        D2MF_STR_POST(cNE, x14, x3, 4);                                        \
        TSTS_IMM8(x2, 2);                                                      \
        D2MF_STRB_POST(cNE, x14, x3, 1); D2MF_STRB_POST(cNE, x14, x3, 1);      \
        TSTS_IMM8(x2, 1);                                                      \
        D2MF_STRB_POST(cNE, x14, x3, 1);                                       \
    } else {                                                                   \
        if(dyn86_membase) { ADD_IMM8_ROR(x1, x1, mf_mb, 4); }                  \
        TSTS_IMM8(x2, 32); D2MF_SKIP(4);                                       \
        D2MF_VLD1Q_8_W(0, x1); D2MF_VLD1Q_8_W(2, x1);                          \
        D2MF_VST1Q_8_W(0, x3); D2MF_VST1Q_8_W(2, x3);                          \
        TSTS_IMM8(x2, 16); D2MF_SKIP(2);                                       \
        D2MF_VLD1Q_8_W(0, x1); D2MF_VST1Q_8_W(0, x3);                          \
        TSTS_IMM8(x2, 8); D2MF_SKIP(2);                                        \
        D2MF_VLD1_8_W(0, x1);  D2MF_VST1_8_W(0, x3);                           \
        TSTS_IMM8(x2, 4);                                                      \
        D2MF_LDR_POST(cNE, x14, x1, 4);  D2MF_STR_POST(cNE, x14, x3, 4);       \
        TSTS_IMM8(x2, 2);                                                      \
        D2MF_LDRB_POST(cNE, x14, x1, 1); D2MF_STRB_POST(cNE, x14, x3, 1);      \
        D2MF_LDRB_POST(cNE, x14, x1, 1); D2MF_STRB_POST(cNE, x14, x3, 1);      \
        TSTS_IMM8(x2, 1);                                                      \
        D2MF_LDRB_POST(cNE, x14, x1, 1); D2MF_STRB_POST(cNE, x14, x3, 1);      \
    }                                                                          \
    if(dyn86_mi_fastchk) {                 /* oracle POST (check build only)*/ \
        MOV_REG(x1, xESP);                                                     \
        CALL_((is_set)?(void*)dyn86_mi_fast_post_set:(void*)dyn86_mi_fast_post_cpy, -1, 0); \
    }                                                                          \
    ret_to_epilog(dyn, ninst);             /* the guest RET                 */ \
    dyn->memfast_mark = dyn->arm_size;     /* <- the helper-call sequence   */ \
    if(STEP == 3) ++dyn86_mi_fast_blocks;  /* only the pass that WRITES     */ \
} while(0)

#endif /* DYN86_MEMFAST_H_ */
