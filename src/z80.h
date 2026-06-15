#pragma once
#include "types.h"

/* Z80 flag bits */
#define Z80_FLAG_C  0x01   /* Carry */
#define Z80_FLAG_N  0x02   /* Add/Subtract */
#define Z80_FLAG_PV 0x04   /* Parity/Overflow */
#define Z80_FLAG_H  0x10   /* Half Carry */
#define Z80_FLAG_Z  0x40   /* Zero */
#define Z80_FLAG_S  0x80   /* Sign */

typedef struct {
    /* Main register pairs */
    union { struct { u8 f, a; }; u16 af; };
    union { struct { u8 c, b; }; u16 bc; };
    union { struct { u8 e, d; }; u16 de; };
    union { struct { u8 l, h; }; u16 hl; };

    /* Alternate register pairs */
    u16 af_, bc_, de_, hl_;

    /* Index registers and stack pointer */
    u16 ix, iy, sp, pc;

    /* Interrupt / refresh */
    u8  i, r;
    bool iff1, iff2;
    bool ei_delay;     /* EI blocks interrupt for one following instruction */
    u8   im;           /* interrupt mode 0/1/2 */
    bool halted;
    bool pending_irq;
    /* Set true by z80_step in the cycle it ACCEPTS a maskable interrupt
     * (so the caller can ack the IRQ source, e.g. clear GA bit 5).
     * Caller is expected to consume by clearing back to false. */
    bool int_accepted;

    int cycles;        /* T-states consumed this step */
    u8   last_op;      /* opcode just fetched by z80_step (for CPC cc_op[] lookup) */
    u8   last_prefix;  /* 0, 0xCB, 0xED, 0xDD, 0xFD — for prefix-table dispatch */

    /* Pipeline-state hint left by the previous instruction. Set to 1 by
     * z80_step when the last opcode was a 16-bit INC/DEC rr, EX (SP),rr,
     * LD SP,rr, a non-taken RET cc, or a repeating CPIR/CPDR; 0 otherwise.
     * Consumed at the top of the next z80_step_impl: when an IRQ is
     * accepted and iWSAdjust > 0, the accept cost is reduced by 4 cycles,
     * mirroring Caprice32/konCePCja/qcpcemu. Goes hand-in-hand with the
     * IM1=20 / IM2=28 baseline costs they all share — IM1 effective cost
     * is then 16 or 20 (context dependent), IM2 is 24 or 28. Closes the
     * residual ~10% HDCPM boot race left by #102.
     *
     * NOTE: LDIR/LDDR repeats do NOT bump iWSAdjust per Caprice32's
     * macros at z80.cpp:839-846 / 860-866; only CPIR/CPDR do
     * (z80.cpp:753-760 / 775-782). */
    int iWSAdjust;
} Z80;

/* Bus callbacks — implemented by cpc.c, wiring mem + I/O */
typedef struct {
    u8   (*mem_read) (void *ctx, u16 addr);
    void (*mem_write)(void *ctx, u16 addr, u8 val);
    u8   (*io_read)  (void *ctx, u16 port);
    void (*io_write) (void *ctx, u16 port, u8 val);
    /* Mid-instruction bus arbiter (mirror of konCePCja's
     * z80_wait_states pattern). z80_step() zeroes *ticked_in_step
     * before each instruction; IO opcodes may call tick(N) to advance
     * the bus state by N cycles BEFORE the IO write completes, which
     * also bumps *ticked_in_step. The outer cpc_frame() loop then
     * subtracts ticked_in_step from the instruction's total before
     * advancing the bus for the remaining post-IO cycles. NULL
     * callbacks mean "no mid-step ticking" — preserves old behavior. */
    void (*tick)    (void *ctx, int cycles);
    int  *ticked_in_step;
    void *ctx;
} Z80Bus;

void z80_init(Z80 *cpu);
void z80_reset(Z80 *cpu);
int  z80_step(Z80 *cpu, Z80Bus *bus);   /* Execute one instruction; returns T-states */
void z80_interrupt(Z80 *cpu);           /* Assert maskable interrupt */
void z80_nmi(Z80 *cpu);

/* Optional pre-execution hook for RST #10 (the SymbOS message-send vector).
 * NULL = no hook (zero overhead). Set by symbos_trace.c when --trace-symbos-msg
 * is given. The hook is called with the CPU + bus right before RST #10 runs;
 * it must not modify CPU state. */
extern void (*z80_rst10_hook)(Z80 *cpu, Z80Bus *bus);
