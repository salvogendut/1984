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

    int cycles;        /* T-states consumed this step */
} Z80;

/* Bus callbacks — implemented by cpc.c, wiring mem + I/O */
typedef struct {
    u8   (*mem_read) (void *ctx, u16 addr);
    void (*mem_write)(void *ctx, u16 addr, u8 val);
    u8   (*io_read)  (void *ctx, u16 port);
    void (*io_write) (void *ctx, u16 port, u8 val);
    void *ctx;
} Z80Bus;

void z80_init(Z80 *cpu);
void z80_reset(Z80 *cpu);
int  z80_step(Z80 *cpu, Z80Bus *bus);   /* Execute one instruction; returns T-states */
void z80_interrupt(Z80 *cpu);           /* Assert maskable interrupt */
void z80_nmi(Z80 *cpu);
