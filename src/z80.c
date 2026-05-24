#include "z80.h"
#include <string.h>
#include <stdio.h>

/* Helpers */
#define READ8(addr)       bus->mem_read(bus->ctx, (addr))
#define WRITE8(addr, v)   bus->mem_write(bus->ctx, (addr), (v))
#define IN(port)          bus->io_read(bus->ctx, (port))
#define OUT(port, v)      bus->io_write(bus->ctx, (port), (v))

#define FETCH8()          READ8(cpu->pc++)
#define FETCH16()         (cpu->pc += 2, READ8(cpu->pc-2) | (READ8(cpu->pc-1) << 8))

#define SET_FLAG(f)       cpu->f |= (f)
#define CLR_FLAG(f)       cpu->f &= ~(f)
#define TST_FLAG(f)       (cpu->f & (f))
#define SET_FLAGS(mask,v) cpu->f = (cpu->f & ~(mask)) | ((v) & (mask))

static inline u16 read16(Z80Bus *bus, u16 addr) {
    return READ8(addr) | (READ8(addr + 1) << 8);
}
static inline void write16(Z80Bus *bus, u16 addr, u16 v) {
    WRITE8(addr, v & 0xFF);
    WRITE8(addr + 1, v >> 8);
}
static inline void push16(Z80 *cpu, Z80Bus *bus, u16 v) {
    cpu->sp -= 2;
    write16(bus, cpu->sp, v);
}
static inline u16 pop16(Z80 *cpu, Z80Bus *bus) {
    u16 v = read16(bus, cpu->sp);
    cpu->sp += 2;
    return v;
}

/* Flag helpers */
static const u8 parity_table[256] = {
#define P2(n) n, n^1, n^1, n
#define P4(n) P2(n), P2(n^1)
#define P6(n) P4(n), P4(n^1)
    P6(0), P6(1)
};

static inline u8 flags_szp(u8 v) {
    return (v & 0x80) | (v == 0 ? Z80_FLAG_Z : 0) | (parity_table[v] ? Z80_FLAG_PV : 0);
}

static inline u8 add8_flags(u8 a, u8 b, u8 cin) {
    u16 r = a + b + cin;
    u8 f = 0;
    if (r > 0xFF)              f |= Z80_FLAG_C;
    if (!((u8)r))              f |= Z80_FLAG_Z;
    if ((u8)r & 0x80)          f |= Z80_FLAG_S;
    if (((a ^ b ^ (u8)r) & 0x10)) f |= Z80_FLAG_H;
    if ((~(a ^ b) & (a ^ (u8)r)) & 0x80) f |= Z80_FLAG_PV;
    return f;
}

static inline u8 sub8_flags(u8 a, u8 b, u8 bin) {
    u8 f = add8_flags(a, ~b, !bin) ^ Z80_FLAG_C;
    f |= Z80_FLAG_N;
    return f;
}

void z80_init(Z80 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->sp = 0xFFFF;
    cpu->af = 0xFFFF;
}

void z80_reset(Z80 *cpu) {
    cpu->pc  = 0;
    cpu->iff1 = cpu->iff2 = false;
    cpu->im  = 0;
    cpu->halted = false;
}

void z80_interrupt(Z80 *cpu) {
    if (!cpu->iff1) return;
    cpu->halted = false;
    cpu->iff1 = cpu->iff2 = false;
    /* Mode 1: RST 38h */
    cpu->cycles = 13;
}

void z80_nmi(Z80 *cpu) {
    cpu->halted = false;
    cpu->iff1 = false;
}

/* Stub: full Z80 decode goes here — this will grow substantially */
int z80_step(Z80 *cpu, Z80Bus *bus) {
    if (cpu->halted) {
        cpu->r = (cpu->r + 1) & 0x7F;
        return 4;
    }

    cpu->cycles = 0;
    u8 op = FETCH8();
    cpu->r = ((cpu->r + 1) & 0x7F) | (cpu->r & 0x80);

    switch (op) {
        case 0x00: cpu->cycles = 4; break;                            /* NOP */
        case 0x76: cpu->halted = true; cpu->cycles = 4; break;        /* HALT */

        /* LD r, n */
        case 0x06: cpu->b = FETCH8(); cpu->cycles = 7; break;
        case 0x0E: cpu->c = FETCH8(); cpu->cycles = 7; break;
        case 0x16: cpu->d = FETCH8(); cpu->cycles = 7; break;
        case 0x1E: cpu->e = FETCH8(); cpu->cycles = 7; break;
        case 0x26: cpu->h = FETCH8(); cpu->cycles = 7; break;
        case 0x2E: cpu->l = FETCH8(); cpu->cycles = 7; break;
        case 0x3E: cpu->a = FETCH8(); cpu->cycles = 7; break;

        /* LD rr, nn */
        case 0x01: cpu->bc = FETCH16(); cpu->cycles = 10; break;
        case 0x11: cpu->de = FETCH16(); cpu->cycles = 10; break;
        case 0x21: cpu->hl = FETCH16(); cpu->cycles = 10; break;
        case 0x31: cpu->sp = FETCH16(); cpu->cycles = 10; break;

        /* JP nn */
        case 0xC3: cpu->pc = FETCH16(); cpu->cycles = 10; break;

        /* CALL nn */
        case 0xCD: {
            u16 addr = FETCH16();
            push16(cpu, bus, cpu->pc);
            cpu->pc = addr;
            cpu->cycles = 17;
            break;
        }
        /* RET */
        case 0xC9: cpu->pc = pop16(cpu, bus); cpu->cycles = 10; break;

        /* DI / EI */
        case 0xF3: cpu->iff1 = cpu->iff2 = false; cpu->cycles = 4; break;
        case 0xFB: cpu->iff1 = cpu->iff2 = true;  cpu->cycles = 4; break;

        /* OUT (n), A */
        case 0xD3: { u8 n = FETCH8(); OUT((cpu->a << 8) | n, cpu->a); cpu->cycles = 11; break; }

        /* IN A, (n) */
        case 0xDB: { u8 n = FETCH8(); cpu->a = IN((cpu->a << 8) | n); cpu->cycles = 11; break; }

        default:
            fprintf(stderr, "Unimplemented opcode: 0x%02X at PC=0x%04X\n", op, cpu->pc - 1);
            cpu->cycles = 4;
            break;
    }

    return cpu->cycles;
}
