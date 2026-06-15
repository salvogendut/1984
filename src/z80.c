#include "z80.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void (*z80_rst10_hook)(Z80 *cpu, Z80Bus *bus) = NULL;

/* ---- Cycle tables (Caprice32 / konCePCja convention) ----------------
 * Cycle counts are T-states. Lookups: cc_op[op] for unprefixed,
 * cc_cb[op] for CB-prefixed, cc_ed[op] for ED-prefixed,
 * cc_xy[op] for DD/FD-prefixed (followed by a normal opcode),
 * cc_xycb[op] for DD/FD CB-prefixed.
 *
 * cc_ex[op] holds the EXTRA cycles when a conditional branch is
 * taken (JR/JP/CALL/RET cond), or when a block instruction repeats
 * (LDIR, CPIR, INIR, OTIR and their reverse variants).
 *
 * Two macro families parameterise IN/OUT block opcodes — kept as
 * plain integers here:
 *   Oa  = OUT (n),A  / IN A,(n)            (cc_op rows D3/DB)
 *   Ox/Oy/Ix/Iy = ED-prefix block IN/OUT  (cc_ed rows 40–7F and A0–BF)
 *
 * Ported verbatim from /tmp/koncepcja/src/z80.cpp.  Tables are 1:1.
 * ------------------------------------------------------------------ */
/* IO cycle constants — set to konCePCja's split-totals (X = pre-IO
 * portion + X_ = post-IO portion via z80_wait_states). Now SAFE to
 * bump because the matching bus->tick(pre-IO) calls in the IO
 * instruction handlers (OUT (n),A, OUT (C),r, OUTI/OUTD/OTIR/OTDR)
 * advance the bus arbiter mid-instruction so the GA interrupt
 * counter sees the same state at the IO write as konCePCja's
 * split sees. Without the matching pre-tick split, bumping these
 * to split-totals regresses HDCPM IDE block-op timing. */
#define Oa  12   /* OUT (n),A           — pre 8 + post 4 */
#define Ia  12   /* IN A,(n)            — pre 12 + post 0 */
#define Ox   8   /* OUT (C),r — even with cpc_advance_bus covering all
                  * peripherals and matching bus->tick(8) pre-IO split,
                  * bumping Ox to 12 regresses sweep to 0/13. The remaining
                  * gap vs konCePCja must be in the actual IO write
                  * timing relative to bus arbitration — needs reading
                  * the OUT instruction execution more carefully. */
#define Oy  12   /* OUTI/OUTD/OTIR/OTDR — split-total Oy=16 regresses */
#define Ix  12
#define Iy  16

static const u8 cc_op[256] = {
    4, 12,  8,  8,  4,  4,  8,  4,  4, 12,  8,  8,  4,  4,  8,  4,
   12, 12,  8,  8,  4,  4,  8,  4, 12, 12,  8,  8,  4,  4,  8,  4,
    8, 12, 20,  8,  4,  4,  8,  4,  8, 12, 20,  8,  4,  4,  8,  4,
    8, 12, 16,  8, 12, 12, 12,  4,  8, 12, 16,  8,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    8,  8,  8,  8,  8,  8,  4,  8,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    8, 12, 12, 12, 12, 16,  8, 16,  8, 12, 12,  4, 12, 20,  8, 16,
    8, 12, 12, Oa, 12, 16,  8, 16,  8,  4, 12, Ia, 12,  4,  8, 16,
    8, 12, 12, 24, 12, 16,  8, 16,  8,  4, 12,  4, 12,  4,  8, 16,
    8, 12, 12,  4, 12, 16,  8, 16,  8,  8, 12,  4, 12,  4,  8, 16
};

static const u8 cc_cb[256] = {
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4,
    4,  4,  4,  4,  4,  4, 12,  4,  4,  4,  4,  4,  4,  4, 12,  4
};

static const u8 cc_ed[256] = {
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
   Ix, Ox, 12, 20,  4, 12,  4,  8, Ix, Ox, 12, 20,  4, 12,  4,  8,
   Ix, Ox, 12, 20,  4, 12,  4,  8, Ix, Ox, 12, 20,  4, 12,  4,  8,
   Ix, Ox, 12, 20,  4, 12,  4, 16, Ix, Ox, 12, 20,  4, 12,  4, 16,
   Ix, Ox, 12, 20,  4, 12,  4,  4, Ix, Ox, 12, 20,  4, 12,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
   16, 12, Iy, Oy,  4,  4,  4,  4, 16, 12, Iy, Oy,  4,  4,  4,  4,
   16, 12, Iy, Oy,  4,  4,  4,  4, 16, 12, Iy, Oy,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
    4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4
};

static const u8 cc_xy[256] = {
    4, 12,  8,  8,  4,  4,  8,  4,  4, 12,  8,  8,  4,  4,  8,  4,
   12, 12,  8,  8,  4,  4,  8,  4, 12, 12,  8,  8,  4,  4,  8,  4,
    8, 12, 20,  8,  4,  4,  8,  4,  8, 12, 20,  8,  4,  4,  8,  4,
    8, 12, 16,  8, 20, 20, 20,  4,  8, 12, 16,  8,  4,  4,  8,  4,
    4,  4,  4,  4,  4,  4, 16,  4,  4,  4,  4,  4,  4,  4, 16,  4,
    4,  4,  4,  4,  4,  4, 16,  4,  4,  4,  4,  4,  4,  4, 16,  4,
    4,  4,  4,  4,  4,  4, 16,  4,  4,  4,  4,  4,  4,  4, 16,  4,
   16, 16, 16, 16, 16, 16,  4, 16,  4,  4,  4,  4,  4,  4, 16,  4,
    4,  4,  4,  4,  4,  4, 16,  4,  4,  4,  4,  4,  4,  4, 16,  4,
    4,  4,  4,  4,  4,  4, 16,  4,  4,  4,  4,  4,  4,  4, 16,  4,
    4,  4,  4,  4,  4,  4, 16,  4,  4,  4,  4,  4,  4,  4, 16,  4,
    4,  4,  4,  4,  4,  4, 16,  4,  4,  4,  4,  4,  4,  4, 16,  4,
    8, 12, 12, 12, 12, 16,  8, 16,  8, 12, 12,  4, 12, 20,  8, 16,
    8, 12, 12, Oa, 12, 16,  8, 16,  8,  4, 12, Ia, 12,  4,  8, 16,
    8, 12, 12, 24, 12, 16,  8, 16,  8,  4, 12,  4, 12,  4,  8, 16,
    8, 12, 12,  4, 12, 16,  8, 16,  8,  8, 12,  4, 12,  4,  8, 16
};

__attribute__((unused))
static const u8 cc_xycb[256] = {
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
   16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
   16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
   16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
   20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20
};

static const u8 cc_ex[256] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    4,  0,  0,  0,  0,  0,  0,  0,  4,  0,  0,  0,  0,  0,  0,  0,
    4,  0,  0,  0,  0,  0,  0,  0,  4,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    4,  8,  4,  4,  0,  0,  0,  0,  4,  8,  4,  4,  0,  0,  0,  0,
    8,  0,  0,  0,  8,  0,  0,  0,  8,  0,  0,  0,  8,  0,  0,  0,
    8,  0,  0,  0,  8,  0,  0,  0,  8,  0,  0,  0,  8,  0,  0,  0,
    8,  0,  0,  0,  8,  0,  0,  0,  8,  0,  0,  0,  8,  0,  0,  0,
    8,  0,  0,  0,  8,  0,  0,  0,  8,  0,  0,  0,  8,  0,  0,  0
};

/* ---- Bus access ---- */
#define READ8(addr)      bus->mem_read(bus->ctx, (u16)(addr))
#define WRITE8(addr, v)  bus->mem_write(bus->ctx, (u16)(addr), (u8)(v))
#define IN(port)         bus->io_read(bus->ctx, (u16)(port))
#define OUT(port, v)     bus->io_write(bus->ctx, (u16)(port), (u8)(v))
#define FETCH8()         READ8(cpu->pc++)
#define FETCH16()        fetch16_impl(cpu, bus)

static inline u16 fetch16_impl(Z80 *cpu, Z80Bus *bus) {
    u16 lo = FETCH8(), hi = FETCH8();
    return lo | (hi << 8);
}
static inline u16 read16(Z80Bus *bus, u16 addr) {
    return READ8(addr) | ((u16)READ8((u16)(addr + 1)) << 8);
}
static inline void write16(Z80Bus *bus, u16 addr, u16 v) {
    WRITE8(addr, v & 0xFF);
    WRITE8((u16)(addr + 1), v >> 8);
}
static inline void push16(Z80 *cpu, Z80Bus *bus, u16 v) {
    cpu->sp -= 2; write16(bus, cpu->sp, v);
}
static inline u16 pop16(Z80 *cpu, Z80Bus *bus) {
    u16 v = read16(bus, cpu->sp); cpu->sp += 2; return v;
}

/* ---- Flags ---- */
static inline u8 sz(u8 v) {
    return (v & 0x80) | (!v ? Z80_FLAG_Z : 0);
}
static inline u8 par(u8 v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (~v & 1) ? Z80_FLAG_PV : 0;
}

/* ---- ALU ---- */
static void do_add(Z80 *cpu, u8 val, bool c) {
    u16 r = cpu->a + val + c;
    cpu->f = (r > 0xFF ? Z80_FLAG_C : 0)
           | (!((u8)r) ? Z80_FLAG_Z : 0)
           | ((u8)r & 0x80 ? Z80_FLAG_S : 0)
           | ((cpu->a ^ val ^ (u8)r) & 0x10 ? Z80_FLAG_H : 0)
           | ((~(cpu->a ^ val) & (cpu->a ^ (u8)r)) & 0x80 ? Z80_FLAG_PV : 0);
    cpu->a = (u8)r;
}
static void do_sub(Z80 *cpu, u8 val, bool c) {
    u16 r = (u16)cpu->a - val - c;
    cpu->f = Z80_FLAG_N
           | (r > 0xFF ? Z80_FLAG_C : 0)
           | (!((u8)r) ? Z80_FLAG_Z : 0)
           | ((u8)r & 0x80 ? Z80_FLAG_S : 0)
           | ((cpu->a ^ val ^ (u8)r) & 0x10 ? Z80_FLAG_H : 0)
           | (((cpu->a ^ val) & (cpu->a ^ (u8)r)) & 0x80 ? Z80_FLAG_PV : 0);
    cpu->a = (u8)r;
}
static void do_and(Z80 *cpu, u8 v) { cpu->a &= v; cpu->f = sz(cpu->a) | par(cpu->a) | Z80_FLAG_H; }
static void do_xor(Z80 *cpu, u8 v) { cpu->a ^= v; cpu->f = sz(cpu->a) | par(cpu->a); }
static void do_or (Z80 *cpu, u8 v) { cpu->a |= v; cpu->f = sz(cpu->a) | par(cpu->a); }
static void do_cp (Z80 *cpu, u8 v) { u8 a = cpu->a; do_sub(cpu, v, false); cpu->a = a; }

static u8 do_inc(Z80 *cpu, u8 v) {
    u8 r = v + 1;
    cpu->f = (cpu->f & Z80_FLAG_C) | sz(r)
           | ((v & 0x0F) == 0x0F ? Z80_FLAG_H : 0)
           | (v == 0x7F ? Z80_FLAG_PV : 0);
    return r;
}
static u8 do_dec(Z80 *cpu, u8 v) {
    u8 r = v - 1;
    cpu->f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_N | sz(r)
           | ((v & 0x0F) == 0x00 ? Z80_FLAG_H : 0)
           | (v == 0x80 ? Z80_FLAG_PV : 0);
    return r;
}
static void do_addhl(Z80 *cpu, u16 val) {
    u32 r = cpu->hl + val;
    cpu->f = (cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV))
           | (r > 0xFFFF ? Z80_FLAG_C : 0)
           | ((cpu->hl ^ val ^ (u16)r) & 0x1000 ? Z80_FLAG_H : 0);
    cpu->hl = (u16)r;
}

/* ---- Register index helpers ---- */
static u8 get_r(Z80 *cpu, Z80Bus *bus, int r, u16 hl) {
    switch (r) {
        case 0: return cpu->b; case 1: return cpu->c;
        case 2: return cpu->d; case 3: return cpu->e;
        case 4: return cpu->h; case 5: return cpu->l;
        case 6: return READ8(hl);
        case 7: return cpu->a;
    }
    return 0;
}
static void set_r(Z80 *cpu, Z80Bus *bus, int r, u16 hl, u8 v) {
    switch (r) {
        case 0: cpu->b = v; break; case 1: cpu->c = v; break;
        case 2: cpu->d = v; break; case 3: cpu->e = v; break;
        case 4: cpu->h = v; break; case 5: cpu->l = v; break;
        case 6: WRITE8(hl, v); break;
        case 7: cpu->a = v; break;
    }
}
static u16 get_rr(Z80 *cpu, int rr) {
    switch (rr) {
        case 0: return cpu->bc; case 1: return cpu->de;
        case 2: return cpu->hl; case 3: return cpu->sp;
    }
    return 0;
}
static void set_rr(Z80 *cpu, int rr, u16 v) {
    switch (rr) {
        case 0: cpu->bc = v; break; case 1: cpu->de = v; break;
        case 2: cpu->hl = v; break; case 3: cpu->sp = v; break;
    }
}
static bool cc(Z80 *cpu, int c) {
    switch (c & 7) {
        case 0: return !(cpu->f & Z80_FLAG_Z);  case 1: return  (cpu->f & Z80_FLAG_Z);
        case 2: return !(cpu->f & Z80_FLAG_C);  case 3: return  (cpu->f & Z80_FLAG_C);
        case 4: return !(cpu->f & Z80_FLAG_PV); case 5: return  (cpu->f & Z80_FLAG_PV);
        case 6: return !(cpu->f & Z80_FLAG_S);  case 7: return  (cpu->f & Z80_FLAG_S);
    }
    return false;
}

/* ---- Rotate/shift helpers (used by CB and main opcodes) ---- */
static u8 do_rlc(Z80 *cpu, u8 v) { u8 r=(v<<1)|(v>>7); cpu->f=sz(r)|par(r)|(v>>7?Z80_FLAG_C:0); return r; }
static u8 do_rrc(Z80 *cpu, u8 v) { u8 r=(v>>1)|(v<<7); cpu->f=sz(r)|par(r)|(v&1?Z80_FLAG_C:0); return r; }
static u8 do_rl (Z80 *cpu, u8 v) { u8 c=(cpu->f&Z80_FLAG_C)?1:0; u8 r=(v<<1)|c; cpu->f=sz(r)|par(r)|(v>>7?Z80_FLAG_C:0); return r; }
static u8 do_rr (Z80 *cpu, u8 v) { u8 c=(cpu->f&Z80_FLAG_C)?0x80:0; u8 r=(v>>1)|c; cpu->f=sz(r)|par(r)|(v&1?Z80_FLAG_C:0); return r; }
static u8 do_sla(Z80 *cpu, u8 v) { u8 r=v<<1;         cpu->f=sz(r)|par(r)|(v>>7?Z80_FLAG_C:0); return r; }
static u8 do_sra(Z80 *cpu, u8 v) { u8 r=(v>>1)|(v&0x80); cpu->f=sz(r)|par(r)|(v&1?Z80_FLAG_C:0); return r; }
static u8 do_srl(Z80 *cpu, u8 v) { u8 r=v>>1;         cpu->f=sz(r)|par(r)|(v&1?Z80_FLAG_C:0); return r; }
static u8 do_sll(Z80 *cpu, u8 v) { u8 r=(v<<1)|1;     cpu->f=sz(r)|par(r)|(v>>7?Z80_FLAG_C:0); return r; }

/* ---- CB prefix ---- */
static int exec_cb(Z80 *cpu, Z80Bus *bus) {
    u8 op = FETCH8();
    cpu->last_op = op;
    cpu->r = ((cpu->r + 1) & 0x7F) | (cpu->r & 0x80);
    int ri = op & 7, b = (op >> 3) & 7;
    u8 v = get_r(cpu, bus, ri, cpu->hl);
    int cyc = (ri == 6) ? 15 : 8;

    if (op < 0x40) {
        switch (b) {
            case 0: v=do_rlc(cpu,v); break; case 1: v=do_rrc(cpu,v); break;
            case 2: v=do_rl (cpu,v); break; case 3: v=do_rr (cpu,v); break;
            case 4: v=do_sla(cpu,v); break; case 5: v=do_sra(cpu,v); break;
            case 6: v=do_sll(cpu,v); break; case 7: v=do_srl(cpu,v); break;
        }
        set_r(cpu, bus, ri, cpu->hl, v);
    } else if (op < 0x80) {
        u8 mask = 1 << b;
        cpu->f = (cpu->f & Z80_FLAG_C) | Z80_FLAG_H
               | (!(v & mask) ? Z80_FLAG_Z | Z80_FLAG_PV : 0)
               | (b == 7 && (v & mask) ? Z80_FLAG_S : 0);
        cyc = (ri == 6) ? 12 : 8;
    } else if (op < 0xC0) {
        set_r(cpu, bus, ri, cpu->hl, v & ~(1 << b));
    } else {
        set_r(cpu, bus, ri, cpu->hl, v | (1 << b));
    }
    return cyc;
}

/* ---- ED prefix ---- */
static int exec_ed(Z80 *cpu, Z80Bus *bus) {
    u8 op = FETCH8();
    cpu->last_op = op;
    cpu->r = ((cpu->r + 1) & 0x7F) | (cpu->r & 0x80);

    switch (op) {
        /* IN r,(C) */
        case 0x40: case 0x48: case 0x50: case 0x58:
        case 0x60: case 0x68: case 0x70: case 0x78: {
            int ri = (op >> 3) & 7; u8 v = IN(cpu->bc);
            if (ri != 6) set_r(cpu, bus, ri, cpu->hl, v);
            cpu->f = (cpu->f & Z80_FLAG_C) | sz(v) | par(v);
            return 12;
        }
        case 0x41: case 0x49: case 0x51: case 0x59:
        case 0x61: case 0x69: case 0x71: case 0x79: {
            int ri = (op >> 3) & 7;
            OUT(cpu->bc, ri == 6 ? 0 : get_r(cpu, bus, ri, cpu->hl));
            return 12;
        }
        /* SBC HL,rr */
        case 0x42: case 0x52: case 0x62: case 0x72: {
            u16 val = get_rr(cpu, (op >> 4) & 3);
            u32 r = (u32)cpu->hl - val - ((cpu->f & Z80_FLAG_C) ? 1 : 0);
            cpu->f = Z80_FLAG_N
                   | (r > 0xFFFF ? Z80_FLAG_C : 0)
                   | (!(u16)r ? Z80_FLAG_Z : 0)
                   | ((u16)r & 0x8000 ? Z80_FLAG_S : 0)
                   | ((cpu->hl ^ val ^ (u16)r) & 0x1000 ? Z80_FLAG_H : 0)
                   | (((cpu->hl ^ val) & (cpu->hl ^ (u16)r)) >> 8 & 0x80 ? Z80_FLAG_PV : 0);
            cpu->hl = (u16)r; return 15;
        }
        /* ADC HL,rr */
        case 0x4A: case 0x5A: case 0x6A: case 0x7A: {
            u16 val = get_rr(cpu, (op >> 4) & 3);
            u32 r = (u32)cpu->hl + val + ((cpu->f & Z80_FLAG_C) ? 1 : 0);
            cpu->f = (r > 0xFFFF ? Z80_FLAG_C : 0)
                   | (!(u16)r ? Z80_FLAG_Z : 0)
                   | ((u16)r & 0x8000 ? Z80_FLAG_S : 0)
                   | ((cpu->hl ^ val ^ (u16)r) & 0x1000 ? Z80_FLAG_H : 0)
                   | ((~(cpu->hl ^ val) & (val ^ (u16)r)) & 0x8000 ? Z80_FLAG_PV : 0);
            cpu->hl = (u16)r; return 15;
        }
        /* LD (nn),rr */
        case 0x43: case 0x53: case 0x63: case 0x73:
            write16(bus, FETCH16(), get_rr(cpu, (op >> 4) & 3)); return 20;
        /* LD rr,(nn) */
        case 0x4B: case 0x5B: case 0x6B: case 0x7B:
            set_rr(cpu, (op >> 4) & 3, read16(bus, FETCH16())); return 20;
        /* NEG */
        case 0x44: case 0x4C: case 0x54: case 0x5C:
        case 0x64: case 0x6C: case 0x74: case 0x7C: {
            u8 a = cpu->a; cpu->a = 0; do_sub(cpu, a, false); return 8;
        }
        case 0x45: case 0x55: case 0x65: case 0x75: /* RETN */
            cpu->iff1 = cpu->iff2; cpu->pc = pop16(cpu, bus); return 14;
        case 0x4D: /* RETI — match konCePCja/Caprice32: restore IFF1 from IFF2 like RETN.
                   * Standard Z80 docs say RETI only restores PC, but real-world
                   * emulators (Caprice32, WinAPE, MAME, FUSE) all restore IFF1
                   * too — software (notably the Amstrad firmware ISR) relies on
                   * IRQs being re-enabled after RETI without an explicit EI. */
            cpu->iff1 = cpu->iff2; cpu->pc = pop16(cpu, bus); return 14;
        case 0x46: case 0x4E: case 0x66: case 0x6E: cpu->im = 0; return 8;
        case 0x56: case 0x76: cpu->im = 1; return 8;
        case 0x5E: case 0x7E: cpu->im = 2; return 8;
        case 0x47: cpu->i = cpu->a; return 9;
        case 0x4F: cpu->r = cpu->a; return 9;
        case 0x57: cpu->a = cpu->i; cpu->f = (cpu->f & Z80_FLAG_C) | sz(cpu->a) | (cpu->iff2 ? Z80_FLAG_PV : 0); return 9;
        case 0x5F: cpu->a = cpu->r; cpu->f = (cpu->f & Z80_FLAG_C) | sz(cpu->a) | (cpu->iff2 ? Z80_FLAG_PV : 0); return 9;
        case 0x67: { u8 m=READ8(cpu->hl); WRITE8(cpu->hl,(m>>4)|(cpu->a<<4)); cpu->a=(cpu->a&0xF0)|(m&0x0F); cpu->f=(cpu->f&Z80_FLAG_C)|sz(cpu->a)|par(cpu->a); return 18; }
        case 0x6F: { u8 m=READ8(cpu->hl); WRITE8(cpu->hl,(m<<4)|(cpu->a&0x0F)); cpu->a=(cpu->a&0xF0)|(m>>4); cpu->f=(cpu->f&Z80_FLAG_C)|sz(cpu->a)|par(cpu->a); return 18; }
        /* Block moves */
        case 0xA0: WRITE8(cpu->de,READ8(cpu->hl)); cpu->hl++; cpu->de++; cpu->bc--; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_C))|(cpu->bc?Z80_FLAG_PV:0); return 16;
        case 0xA8: WRITE8(cpu->de,READ8(cpu->hl)); cpu->hl--; cpu->de--; cpu->bc--; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_C))|(cpu->bc?Z80_FLAG_PV:0); return 16;
        case 0xB0: WRITE8(cpu->de,READ8(cpu->hl)); cpu->hl++; cpu->de++; cpu->bc--; cpu->f&=~Z80_FLAG_PV; if(cpu->bc){cpu->pc-=2;return 21;} return 16;
        case 0xB8: WRITE8(cpu->de,READ8(cpu->hl)); cpu->hl--; cpu->de--; cpu->bc--; cpu->f&=~Z80_FLAG_PV; if(cpu->bc){cpu->pc-=2;return 21;} return 16;
        /* Block compares */
        case 0xA1: { u8 v=READ8(cpu->hl++); u8 r=cpu->a-v; cpu->bc--; cpu->f=(cpu->f&Z80_FLAG_C)|Z80_FLAG_N|sz(r)|((cpu->a^v^r)&0x10?Z80_FLAG_H:0)|(cpu->bc?Z80_FLAG_PV:0); return 16; }
        case 0xA9: { u8 v=READ8(cpu->hl--); u8 r=cpu->a-v; cpu->bc--; cpu->f=(cpu->f&Z80_FLAG_C)|Z80_FLAG_N|sz(r)|((cpu->a^v^r)&0x10?Z80_FLAG_H:0)|(cpu->bc?Z80_FLAG_PV:0); return 16; }
        case 0xB1: { u8 v=READ8(cpu->hl++); u8 r=cpu->a-v; cpu->bc--; cpu->f=(cpu->f&Z80_FLAG_C)|Z80_FLAG_N|sz(r)|((cpu->a^v^r)&0x10?Z80_FLAG_H:0)|(cpu->bc?Z80_FLAG_PV:0); if(cpu->bc&&r){cpu->pc-=2;return 21;} return 16; }
        case 0xB9: { u8 v=READ8(cpu->hl--); u8 r=cpu->a-v; cpu->bc--; cpu->f=(cpu->f&Z80_FLAG_C)|Z80_FLAG_N|sz(r)|((cpu->a^v^r)&0x10?Z80_FLAG_H:0)|(cpu->bc?Z80_FLAG_PV:0); if(cpu->bc&&r){cpu->pc-=2;return 21;} return 16; }
        /* Block I/O — IN ops: 16 cycles total, no pre-IO tick needed
         * (konCePCja Iy=16 + Iy_=0). OUT ops: pre-tick 4 cycles before
         * the IO write to mirror konCePCja's Oy=12 + Oy_=4 split. */
        case 0xA2: { u8 v=IN(cpu->bc); WRITE8(cpu->hl++,v); cpu->b--; cpu->f=sz(cpu->b)|Z80_FLAG_N; return 16; }
        case 0xAA: { u8 v=IN(cpu->bc); WRITE8(cpu->hl--,v); cpu->b--; cpu->f=sz(cpu->b)|Z80_FLAG_N; return 16; }
        case 0xB2: { u8 v=IN(cpu->bc); WRITE8(cpu->hl++,v); cpu->b--; cpu->f=Z80_FLAG_Z|Z80_FLAG_N; if(cpu->b){cpu->pc-=2;return 21;} return 16; }
        case 0xBA: { u8 v=IN(cpu->bc); WRITE8(cpu->hl--,v); cpu->b--; cpu->f=Z80_FLAG_Z|Z80_FLAG_N; if(cpu->b){cpu->pc-=2;return 21;} return 16; }
        case 0xA3: { u8 v=READ8(cpu->hl++); cpu->b--; OUT(cpu->bc,v); cpu->f=sz(cpu->b)|Z80_FLAG_N; return 16; }
        case 0xAB: { u8 v=READ8(cpu->hl--); cpu->b--; OUT(cpu->bc,v); cpu->f=sz(cpu->b)|Z80_FLAG_N; return 16; }
        case 0xB3: { u8 v=READ8(cpu->hl++); cpu->b--; OUT(cpu->bc,v); cpu->f=Z80_FLAG_Z|Z80_FLAG_N; if(cpu->b){cpu->pc-=2;return 21;} return 16; }
        case 0xBB: { u8 v=READ8(cpu->hl--); cpu->b--; OUT(cpu->bc,v); cpu->f=Z80_FLAG_Z|Z80_FLAG_N; if(cpu->b){cpu->pc-=2;return 21;} return 16; }
        default:
            /* All undefined ED-prefix opcodes are documented to behave as
             * 2-byte 8-T-state NOPs on real Z80 hardware. CP/M+ kernels
             * (and some compilers) emit them deliberately as padding or
             * cross-version stubs — don't log, just NOP. */
            return 8;
    }
}

/* ---- DD/FD prefix (IX/IY) ---- */
static int exec_xy(Z80 *cpu, Z80Bus *bus, u16 *xy) {
    u8 op = FETCH8();
    cpu->last_op = op;
    cpu->r = ((cpu->r + 1) & 0x7F) | (cpu->r & 0x80);

    switch (op) {
        case 0x09: { u32 r=*xy+cpu->bc; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|(r>0xFFFF?Z80_FLAG_C:0)|((*xy^cpu->bc^(u16)r)&0x1000?Z80_FLAG_H:0); *xy=(u16)r; return 15; }
        case 0x19: { u32 r=*xy+cpu->de; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|(r>0xFFFF?Z80_FLAG_C:0)|((*xy^cpu->de^(u16)r)&0x1000?Z80_FLAG_H:0); *xy=(u16)r; return 15; }
        case 0x29: { u32 r=(u32)*xy+*xy; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|(r>0xFFFF?Z80_FLAG_C:0); *xy=(u16)r; return 15; }
        case 0x39: { u32 r=*xy+cpu->sp; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|(r>0xFFFF?Z80_FLAG_C:0)|((*xy^cpu->sp^(u16)r)&0x1000?Z80_FLAG_H:0); *xy=(u16)r; return 15; }
        case 0x21: *xy=FETCH16(); return 14;
        case 0x22: write16(bus,FETCH16(),*xy); return 20;
        case 0x2A: *xy=read16(bus,FETCH16()); return 20;
        case 0x23: (*xy)++; return 10;
        case 0x2B: (*xy)--; return 10;
        case 0xE1: *xy=pop16(cpu,bus); return 14;
        case 0xE5: push16(cpu,bus,*xy); return 15;
        case 0xE9: cpu->pc=*xy; return 8;
        case 0xF9: cpu->sp=*xy; return 10;
        case 0xE3: { u16 t=read16(bus,cpu->sp); write16(bus,cpu->sp,*xy); *xy=t; return 23; }
        case 0x34: { s8 d=(s8)FETCH8(); u16 a=(u16)(*xy+d); WRITE8(a,do_inc(cpu,READ8(a))); return 23; }
        case 0x35: { s8 d=(s8)FETCH8(); u16 a=(u16)(*xy+d); WRITE8(a,do_dec(cpu,READ8(a))); return 23; }
        case 0x36: { s8 d=(s8)FETCH8(); u8 n=FETCH8(); WRITE8((u16)(*xy+d),n); return 19; }
        /* LD r,(XY+d) */
        case 0x46: case 0x4E: case 0x56: case 0x5E:
        case 0x66: case 0x6E:           case 0x7E: {
            s8 d=(s8)FETCH8(); set_r(cpu,bus,(op>>3)&7,cpu->hl,READ8((u16)(*xy+d))); return 19;
        }
        /* LD (XY+d),r */
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75:           case 0x77: {
            s8 d=(s8)FETCH8(); WRITE8((u16)(*xy+d),get_r(cpu,bus,op&7,cpu->hl)); return 19;
        }
        /* ALU (XY+d) */
        case 0x86: { s8 d=(s8)FETCH8(); do_add(cpu,READ8((u16)(*xy+d)),false); return 19; }
        case 0x8E: { s8 d=(s8)FETCH8(); do_add(cpu,READ8((u16)(*xy+d)),(cpu->f&Z80_FLAG_C)!=0); return 19; }
        case 0x96: { s8 d=(s8)FETCH8(); do_sub(cpu,READ8((u16)(*xy+d)),false); return 19; }
        case 0x9E: { s8 d=(s8)FETCH8(); do_sub(cpu,READ8((u16)(*xy+d)),(cpu->f&Z80_FLAG_C)!=0); return 19; }
        case 0xA6: { s8 d=(s8)FETCH8(); do_and(cpu,READ8((u16)(*xy+d))); return 19; }
        case 0xAE: { s8 d=(s8)FETCH8(); do_xor(cpu,READ8((u16)(*xy+d))); return 19; }
        case 0xB6: { s8 d=(s8)FETCH8(); do_or (cpu,READ8((u16)(*xy+d))); return 19; }
        case 0xBE: { s8 d=(s8)FETCH8(); do_cp (cpu,READ8((u16)(*xy+d))); return 19; }
        /* DDCB / FDCB */
        case 0xCB: {
            s8 d = (s8)FETCH8(); u8 cb = FETCH8();
            u16 a = (u16)(*xy + d); u8 v = READ8(a);
            int b = (cb >> 3) & 7;
            if (cb < 0x40) {
                switch (b) {
                    case 0:v=do_rlc(cpu,v);break; case 1:v=do_rrc(cpu,v);break;
                    case 2:v=do_rl(cpu,v); break; case 3:v=do_rr(cpu,v); break;
                    case 4:v=do_sla(cpu,v);break; case 5:v=do_sra(cpu,v);break;
                    case 6:v=do_sll(cpu,v);break; case 7:v=do_srl(cpu,v);break;
                }
                WRITE8(a,v);
            } else if (cb < 0x80) {
                cpu->f=(cpu->f&Z80_FLAG_C)|Z80_FLAG_H|(!(v&(1<<b))?Z80_FLAG_Z|Z80_FLAG_PV:0);
            } else if (cb < 0xC0) {
                WRITE8(a, v & ~(1 << b));
            } else {
                WRITE8(a, v | (1 << b));
            }
            return 23;
        }
        /* Undocumented: INC/DEC/LD XYH,n / XYL,n */
        case 0x24: { u8 v=do_inc(cpu,(u8)(*xy>>8)); *xy=(*xy&0x00FF)|((u16)v<<8); return 8; }
        case 0x25: { u8 v=do_dec(cpu,(u8)(*xy>>8)); *xy=(*xy&0x00FF)|((u16)v<<8); return 8; }
        case 0x26: { *xy=(*xy&0x00FF)|((u16)FETCH8()<<8); return 11; }
        case 0x2C: { u8 v=do_inc(cpu,(u8)(*xy&0xFF)); *xy=(*xy&0xFF00)|v; return 8; }
        case 0x2D: { u8 v=do_dec(cpu,(u8)(*xy&0xFF)); *xy=(*xy&0xFF00)|v; return 8; }
        case 0x2E: { *xy=(*xy&0xFF00)|FETCH8(); return 11; }
        /* Undocumented: LD r,r' where H→XYH, L→XYL (all non-(HL) LD pairs in 0x40-0x7F) */
        /* Macro helpers: get/set with XY substitution for registers 4(H) and 5(L) */
        #define GETRXY(r) ((r)==4?(u8)(*xy>>8):(r)==5?(u8)(*xy&0xFF):get_r(cpu,bus,(r),cpu->hl))
        #define SETRXY(r,v) do{ if((r)==4)*xy=(*xy&0x00FF)|((u16)(v)<<8); \
                                else if((r)==5)*xy=(*xy&0xFF00)|(v); \
                                else set_r(cpu,bus,(r),cpu->hl,(v)); }while(0)
        /* Row 0x40: LD B,r' */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x47:
        /* Row 0x48: LD C,r' */
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4F:
        /* Row 0x50: LD D,r' */
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x57:
        /* Row 0x58: LD E,r' */
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5F:
        /* Row 0x60: LD XYH,r' */
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x67:
        /* Row 0x68: LD XYL,r' */
        case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6F:
        /* Row 0x78: LD A,r' */
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7F: {
            int dst=(op>>3)&7, src=op&7;
            SETRXY(dst, GETRXY(src));
            return 8;
        }
        /* Undocumented ALU with XYH/XYL operand */
        case 0x84: do_add(cpu,(u8)(*xy>>8),false); return 8;
        case 0x85: do_add(cpu,(u8)(*xy&0xFF),false); return 8;
        case 0x8C: do_add(cpu,(u8)(*xy>>8),(cpu->f&Z80_FLAG_C)!=0); return 8;
        case 0x8D: do_add(cpu,(u8)(*xy&0xFF),(cpu->f&Z80_FLAG_C)!=0); return 8;
        case 0x94: do_sub(cpu,(u8)(*xy>>8),false); return 8;
        case 0x95: do_sub(cpu,(u8)(*xy&0xFF),false); return 8;
        case 0x9C: do_sub(cpu,(u8)(*xy>>8),(cpu->f&Z80_FLAG_C)!=0); return 8;
        case 0x9D: do_sub(cpu,(u8)(*xy&0xFF),(cpu->f&Z80_FLAG_C)!=0); return 8;
        case 0xA4: do_and(cpu,(u8)(*xy>>8)); return 8;
        case 0xA5: do_and(cpu,(u8)(*xy&0xFF)); return 8;
        case 0xAC: do_xor(cpu,(u8)(*xy>>8)); return 8;
        case 0xAD: do_xor(cpu,(u8)(*xy&0xFF)); return 8;
        case 0xB4: do_or(cpu,(u8)(*xy>>8)); return 8;
        case 0xB5: do_or(cpu,(u8)(*xy&0xFF)); return 8;
        case 0xBC: do_cp(cpu,(u8)(*xy>>8)); return 8;
        case 0xBD: do_cp(cpu,(u8)(*xy&0xFF)); return 8;
        /* Unrecognised DD/FD — treat prefix as NOP and re-execute op */
        default:
            cpu->pc--;   /* put the opcode back */
            return 4;
    }
    #undef GETRXY
    #undef SETRXY
}

/* ---- Public API ---- */

void z80_init(Z80 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->sp = 0xFFFF;
    cpu->af = 0xFFFF;
}

void z80_reset(Z80 *cpu) {
    cpu->pc = 0;
    cpu->iff1 = cpu->iff2 = false;
    cpu->im = 0;
    cpu->halted = false;
    cpu->pending_irq = false;
    cpu->int_accepted = false;
    cpu->iWSAdjust = 0;
}

void z80_interrupt(Z80 *cpu) {
    cpu->pending_irq = true;
}

void z80_nmi(Z80 *cpu) {
    cpu->halted = false;
    cpu->iff1 = false;
}

/* Forward decl of the original implementation, defined immediately below. */
static int z80_step_impl(Z80 *cpu, Z80Bus *bus);

/* Public z80_step: thin wrapper around z80_step_impl that, when
 * ONE_K_CC_TABLES is set, overrides the cycle count with the
 * Caprice32/konCePCja-style cc_op[]/cc_cb[]/cc_ed[]/cc_xy[]/cc_xycb[]
 * tables. When off (default), returns the impl's original cycles —
 * zero behaviour change. */
int z80_step(Z80 *cpu, Z80Bus *bus) {
    /* cc_tables default ON since 2026-06-07 (fixes #102). Opt-out
     * for A/B comparison via ONE_K_CC_TABLES=0. */
    static int use_tables = -1;
    if (use_tables == -1) {
        const char *e = getenv("ONE_K_CC_TABLES");
        use_tables = (e && e[0] == '0') ? 0 : 1;
    }

    /* Reset the mid-step bus-tick counter before each instruction so IO
     * opcodes can advance the bus arbiter partway through (see
     * Z80Bus::tick / Z80Bus::ticked_in_step comments). */
    if (bus->ticked_in_step) *bus->ticked_in_step = 0;

    if (!use_tables)
        return z80_step_impl(cpu, bus);

    /* Snapshot pre-state so we can detect taken conditional branches. */
    u16 pc_before = cpu->pc;
    u16 sp_before = cpu->sp;
    u8  b_before  = cpu->b;
    int orig = z80_step_impl(cpu, bus);

    /* IRQ acceptance and HALT short-circuit return early from impl;
     * trust the impl's count in those cases. */
    if (cpu->int_accepted || (cpu->halted && cpu->pc == pc_before))
        return orig;

    u8 op = cpu->last_op;
    int cycles;
    switch (cpu->last_prefix) {
    case 0xCB: cycles = cc_cb[op];   break;
    case 0xED: cycles = cc_ed[op];   break;
    case 0xDD: case 0xFD: cycles = cc_xy[op]; break;
    default:   cycles = cc_op[op];   break;
    }

    /* cc_ex[]: extra T-states when a conditional branch is taken or a
     * block instruction repeats. Only relevant for the unprefixed and
     * ED tables; CB/xy/xycb sub-tables don't have these. */
    bool taken = false;
    if (cpu->last_prefix == 0) {
        switch (op) {
        case 0x10:                            /* DJNZ d        */
            taken = (cpu->b != 0);            /* B was decremented */
            break;
        case 0x20: case 0x28: case 0x30: case 0x38:  /* JR cc,d */
            taken = (cpu->pc != (u16)(pc_before + 2));
            break;
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:  /* RET cc */
        case 0xE0: case 0xE8: case 0xF0: case 0xF8:
            taken = (cpu->sp == (u16)(sp_before + 2));
            break;
        case 0xC4: case 0xCC: case 0xD4: case 0xDC:  /* CALL cc,nn */
        case 0xE4: case 0xEC: case 0xF4: case 0xFC:
            taken = (cpu->sp == (u16)(sp_before - 2));
            break;
        }
    } else if (cpu->last_prefix == 0xED) {
        /* Block ops: LDIR/LDDR/CPIR/CPDR/INIR/INDR/OTIR/OTDR. They
         * "repeat" by rolling PC back by 2 inside the impl, so taken
         * means PC went backwards. */
        switch (op) {
        case 0xB0: case 0xB8:                 /* LDIR, LDDR */
        case 0xB1: case 0xB9:                 /* CPIR, CPDR */
        case 0xB2: case 0xBA:                 /* INIR, INDR */
        case 0xB3: case 0xBB:                 /* OTIR, OTDR */
            taken = (cpu->pc != (u16)(pc_before + 2));
            break;
        }
    }
    if (taken)
        cycles += cc_ex[op];

    /* iWSAdjust pipeline hint for the next instruction's IRQ accept.
     * Mirrors Caprice32: 16-bit INC/DEC rr, EX (SP),rr, LD SP,rr, a
     * non-taken RET cc, or a repeating CPIR/CPDR all shave 4 cycles
     * off the next IM1/IM2 accept cost. Crucially LDIR/LDDR and
     * block-IO repeats do NOT bump (caprice32 z80.cpp:839-846, 860-866). */
    bool bump = false;
    if (cpu->last_prefix == 0) {
        switch (op) {
        case 0x03: case 0x0B: case 0x13: case 0x1B:
        case 0x23: case 0x2B: case 0x33: case 0x3B:   /* INC/DEC rr (BC/DE/HL/SP) */
        case 0xE3:                                    /* EX (SP),HL              */
        case 0xF9:                                    /* LD SP,HL                */
            bump = true; break;
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:   /* RET cc — bump when NOT taken */
        case 0xE0: case 0xE8: case 0xF0: case 0xF8:
            bump = !taken; break;
        }
    } else if (cpu->last_prefix == 0xDD || cpu->last_prefix == 0xFD) {
        switch (op) {
        case 0x23: case 0x2B:                          /* INC/DEC IX/IY          */
        case 0x03: case 0x0B: case 0x13: case 0x1B:    /* INC/DEC BC/DE under DD/FD */
        case 0x33: case 0x3B:                          /* INC/DEC SP under DD/FD */
        case 0xE3:                                     /* EX (SP),IX/IY          */
        case 0xF9:                                     /* LD SP,IX/IY            */
            bump = true; break;
        /* RET cc bumps when NOT taken — konCePCja's DD/FD-prefix dispatch
         * has the full RET cc set at z80.cpp:2031-2040 and 2844-2851.
         * 1984 had only the unprefixed RET cc bumps; the DD/FD-prefix
         * forms fall through to the same RET cc impl but were missing
         * the iWSAdjust bump. */
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        case 0xE0: case 0xE8: case 0xF0: case 0xF8:
            bump = !taken; break;
        }
    } else if (cpu->last_prefix == 0xED) {
        switch (op) {
        case 0xB1: case 0xB9:                          /* CPIR, CPDR — bump on repeat */
            bump = taken; break;
        /* LDI/LDD/LDIR/LDDR, LD A,I / LD A,R, LD I,A / LD R,A always bump
         * (konCePCja z80.cpp:2572-2587). 1984 historically left these out
         * and the cycle drift accumulated through any block move — the
         * residual #129 cold-reset window (~frame 3266 with frozen RTC)
         * goes away once the bumps are restored. */
        case 0xA0: case 0xA8:                          /* LDI, LDD            */
        case 0xB0: case 0xB8:                          /* LDIR, LDDR          */
        case 0x57: case 0x5F:                          /* LD A,I / LD A,R     */
        case 0x47: case 0x4F:                          /* LD I,A / LD R,A     */
            bump = true; break;
        }
    }
    cpu->iWSAdjust = bump ? 1 : 0;

    (void)b_before; /* future use if we need pre-state B for DJNZ corner case */
    (void)orig;
    return cycles;
}

static int z80_step_impl(Z80 *cpu, Z80Bus *bus) {
    /* Service maskable interrupt (blocked for one instruction after EI) */
    if (cpu->pending_irq && cpu->iff1 && !cpu->ei_delay) {
        cpu->pending_irq = false;
        cpu->halted = false;
        cpu->iff1 = cpu->iff2 = false;
        cpu->int_accepted = true;
        cpu->r = ((cpu->r + 1) & 0x7F) | (cpu->r & 0x80);
        /* #102 instrumentation: log the PC the Z80 was at when IM1
         * was accepted — that PC is pushed onto the stack as the
         * eventual RETI/RET return address. Gated on ONE_K_TRACE_IM1. */
        {
            extern int g_debug_enabled;
            static int tracing = -1;
            if (tracing == -1)
                tracing = (g_debug_enabled && getenv("ONE_K_TRACE_IM1")) ? 1 : 0;
            if (tracing) {
                extern int cpc_frame_count;
                fprintf(stderr, "[IM1] frame=%d  pushing PC=%04X SP=%04X (next SP=%04X)\n",
                        cpc_frame_count, cpu->pc, cpu->sp,
                        (u16)(cpu->sp - 2));
            }
        }
        push16(cpu, bus, cpu->pc);
        /* IM1 = 20, IM2 = 28 baseline (Caprice32/konCePCja/qcpcemu).
         * iWSAdjust shaves 4 cycles when the previous instruction left
         * the pipeline state where the chip can accept the IRQ faster
         * (16-bit INC/DEC, EX (SP),HL, LD SP,HL, non-taken RET cc,
         * repeating CPIR/CPDR). Effective cost: 16/20 (IM1) or 24/28
         * (IM2) depending on pipeline state. The two values are coupled
         * — applied separately, either regresses HDCPM boot stability;
         * applied together they restore the reference-emulator pacing. */
        int adj = cpu->iWSAdjust ? -4 : 0;
        cpu->iWSAdjust = 0;
        switch (cpu->im) {
            case 0: case 1:
                /* Mid-IRQ-accept bus tick: konCePCja calls z80_wait_states
                 * inside the IRQ accept handler (z80.cpp:1046/1063) so the
                 * GA interrupt counter advances mid-acceptance rather than
                 * all-after. Empirically: removing this tick regresses the
                 * sweep from 8/13 to 4/13. */
                if (bus->tick) bus->tick(bus->ctx, 20 + adj);
                cpu->pc = 0x0038; return 20 + adj;
            case 2:
                if (bus->tick) bus->tick(bus->ctx, 28 + adj);
                cpu->pc = read16(bus, (u16)((cpu->i << 8) | 0xFF));
                return 28 + adj;
        }
    }
    cpu->ei_delay = false;

    if (cpu->halted) {
        cpu->r = (cpu->r + 1) & 0x7F;
        return 4;
    }

    u8 op = FETCH8();
    cpu->last_op = op;
    cpu->last_prefix = 0;
    cpu->r = ((cpu->r + 1) & 0x7F) | (cpu->r & 0x80);

    switch (op) {
        case 0x00: return 4; /* NOP */

        /* LD rr,nn */
        case 0x01: cpu->bc=FETCH16(); return 10;
        case 0x11: cpu->de=FETCH16(); return 10;
        case 0x21: cpu->hl=FETCH16(); return 10;
        case 0x31: cpu->sp=FETCH16(); return 10;

        /* LD r,n */
        case 0x06: cpu->b=FETCH8(); return 7;
        case 0x0E: cpu->c=FETCH8(); return 7;
        case 0x16: cpu->d=FETCH8(); return 7;
        case 0x1E: cpu->e=FETCH8(); return 7;
        case 0x26: cpu->h=FETCH8(); return 7;
        case 0x2E: cpu->l=FETCH8(); return 7;
        case 0x3E: cpu->a=FETCH8(); return 7;
        case 0x36: { u8 n=FETCH8(); WRITE8(cpu->hl,n); return 10; }

        /* LD (rr),A / LD A,(rr) */
        case 0x02: WRITE8(cpu->bc,cpu->a); return 7;
        case 0x12: WRITE8(cpu->de,cpu->a); return 7;
        case 0x32: WRITE8(FETCH16(),cpu->a); return 13;
        case 0x0A: cpu->a=READ8(cpu->bc); return 7;
        case 0x1A: cpu->a=READ8(cpu->de); return 7;
        case 0x3A: cpu->a=READ8(FETCH16()); return 13;

        /* LD (nn),HL / LD HL,(nn) */
        case 0x22: write16(bus,FETCH16(),cpu->hl); return 16;
        case 0x2A: cpu->hl=read16(bus,FETCH16()); return 16;

        /* EX AF,AF' */
        case 0x08: { u16 t=cpu->af; cpu->af=cpu->af_; cpu->af_=t; return 4; }

        /* INC/DEC rr */
        case 0x03: cpu->bc++; return 6;  case 0x0B: cpu->bc--; return 6;
        case 0x13: cpu->de++; return 6;  case 0x1B: cpu->de--; return 6;
        case 0x23: cpu->hl++; return 6;  case 0x2B: cpu->hl--; return 6;
        case 0x33: cpu->sp++; return 6;  case 0x3B: cpu->sp--; return 6;

        /* INC r */
        case 0x04: cpu->b=do_inc(cpu,cpu->b); return 4;
        case 0x0C: cpu->c=do_inc(cpu,cpu->c); return 4;
        case 0x14: cpu->d=do_inc(cpu,cpu->d); return 4;
        case 0x1C: cpu->e=do_inc(cpu,cpu->e); return 4;
        case 0x24: cpu->h=do_inc(cpu,cpu->h); return 4;
        case 0x2C: cpu->l=do_inc(cpu,cpu->l); return 4;
        case 0x3C: cpu->a=do_inc(cpu,cpu->a); return 4;
        case 0x34: { u8 v=do_inc(cpu,READ8(cpu->hl)); WRITE8(cpu->hl,v); return 11; }

        /* DEC r */
        case 0x05: cpu->b=do_dec(cpu,cpu->b); return 4;
        case 0x0D: cpu->c=do_dec(cpu,cpu->c); return 4;
        case 0x15: cpu->d=do_dec(cpu,cpu->d); return 4;
        case 0x1D: cpu->e=do_dec(cpu,cpu->e); return 4;
        case 0x25: cpu->h=do_dec(cpu,cpu->h); return 4;
        case 0x2D: cpu->l=do_dec(cpu,cpu->l); return 4;
        case 0x3D: cpu->a=do_dec(cpu,cpu->a); return 4;
        case 0x35: { u8 v=do_dec(cpu,READ8(cpu->hl)); WRITE8(cpu->hl,v); return 11; }

        /* Rotates (affect C, H=0, N=0 only) */
        case 0x07: { u8 c=cpu->a>>7; cpu->a=(cpu->a<<1)|c; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|(c?Z80_FLAG_C:0); return 4; }
        case 0x0F: { u8 c=cpu->a&1; cpu->a=(cpu->a>>1)|(c<<7); cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|(c?Z80_FLAG_C:0); return 4; }
        case 0x17: { u8 cin=(cpu->f&Z80_FLAG_C)?1:0; u8 c=cpu->a>>7; cpu->a=(cpu->a<<1)|cin; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|(c?Z80_FLAG_C:0); return 4; }
        case 0x1F: { u8 cin=(cpu->f&Z80_FLAG_C)?0x80:0; u8 c=cpu->a&1; cpu->a=(cpu->a>>1)|cin; cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|(c?Z80_FLAG_C:0); return 4; }

        /* ADD HL,rr */
        case 0x09: do_addhl(cpu,cpu->bc); return 11;
        case 0x19: do_addhl(cpu,cpu->de); return 11;
        case 0x29: do_addhl(cpu,cpu->hl); return 11;
        case 0x39: do_addhl(cpu,cpu->sp); return 11;

        /* JR */
        case 0x18: { s8 d=(s8)FETCH8(); cpu->pc+=d; return 12; }
        case 0x20: { s8 d=(s8)FETCH8(); if(!(cpu->f&Z80_FLAG_Z)){cpu->pc+=d;return 12;} return 7; }
        case 0x28: { s8 d=(s8)FETCH8(); if( (cpu->f&Z80_FLAG_Z)){cpu->pc+=d;return 12;} return 7; }
        case 0x30: { s8 d=(s8)FETCH8(); if(!(cpu->f&Z80_FLAG_C)){cpu->pc+=d;return 12;} return 7; }
        case 0x38: { s8 d=(s8)FETCH8(); if( (cpu->f&Z80_FLAG_C)){cpu->pc+=d;return 12;} return 7; }
        case 0x10: { s8 d=(s8)FETCH8(); cpu->b--; if(cpu->b){cpu->pc+=d;return 13;} return 8; } /* DJNZ */

        /* DAA */
        case 0x27: {
            u8 a = cpu->a; bool n=(cpu->f&Z80_FLAG_N), h=(cpu->f&Z80_FLAG_H), c=(cpu->f&Z80_FLAG_C);
            /* Both adjustments decided from the ORIGINAL A. Previously the
             * second check used the already-+6'd value, which mis-handled
             * cases like A=0xFF (used by BASIC's HEX$ routine: it does
             * OR 0xF0 then DAA to fold a nibble into hex-letter digits).
             * Real Z80 fires both +0x06 and +0x60 for that input. */
            u8 diff = 0;
            bool new_c;
            if (h || (a & 0x0F) > 9) diff |= 0x06;
            if (c || a > 0x99)       { diff |= 0x60; new_c = true; } else new_c = c;
            if (n) a -= diff; else a += diff;
            /* New H: bit-4 carry/borrow of the adjustment vs. original A */
            bool new_h = ((cpu->a ^ a) & 0x10) != 0;
            cpu->a = a;
            cpu->f = (cpu->f & Z80_FLAG_N)
                   | sz(a) | par(a)
                   | (new_h ? Z80_FLAG_H : 0)
                   | (new_c ? Z80_FLAG_C : 0);
            return 4;
        }

        /* Misc */
        case 0x2F: cpu->a=~cpu->a; cpu->f|=Z80_FLAG_H|Z80_FLAG_N; return 4; /* CPL */
        case 0x37: cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|Z80_FLAG_C; return 4; /* SCF */
        case 0x3F: cpu->f=(cpu->f&(Z80_FLAG_S|Z80_FLAG_Z|Z80_FLAG_PV))|((cpu->f&Z80_FLAG_C)?Z80_FLAG_H:Z80_FLAG_C); return 4; /* CCF */

        /* LD r,r' block 0x40-0x7F */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:           case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int dst=(op>>3)&7, src=op&7;
            set_r(cpu,bus,dst,cpu->hl,get_r(cpu,bus,src,cpu->hl));
            return (src==6||dst==6)?7:4;
        }
        case 0x76: cpu->halted=true; return 4; /* HALT */

        /* ALU with register (0x80-0xBF) */
        case 0x80:case 0x81:case 0x82:case 0x83:case 0x84:case 0x85:case 0x86:case 0x87:
            do_add(cpu,get_r(cpu,bus,op&7,cpu->hl),false); return (op&7)==6?7:4;
        case 0x88:case 0x89:case 0x8A:case 0x8B:case 0x8C:case 0x8D:case 0x8E:case 0x8F:
            do_add(cpu,get_r(cpu,bus,op&7,cpu->hl),(cpu->f&Z80_FLAG_C)!=0); return (op&7)==6?7:4;
        case 0x90:case 0x91:case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97:
            do_sub(cpu,get_r(cpu,bus,op&7,cpu->hl),false); return (op&7)==6?7:4;
        case 0x98:case 0x99:case 0x9A:case 0x9B:case 0x9C:case 0x9D:case 0x9E:case 0x9F:
            do_sub(cpu,get_r(cpu,bus,op&7,cpu->hl),(cpu->f&Z80_FLAG_C)!=0); return (op&7)==6?7:4;
        case 0xA0:case 0xA1:case 0xA2:case 0xA3:case 0xA4:case 0xA5:case 0xA6:case 0xA7:
            do_and(cpu,get_r(cpu,bus,op&7,cpu->hl)); return (op&7)==6?7:4;
        case 0xA8:case 0xA9:case 0xAA:case 0xAB:case 0xAC:case 0xAD:case 0xAE:case 0xAF:
            do_xor(cpu,get_r(cpu,bus,op&7,cpu->hl)); return (op&7)==6?7:4;
        case 0xB0:case 0xB1:case 0xB2:case 0xB3:case 0xB4:case 0xB5:case 0xB6:case 0xB7:
            do_or (cpu,get_r(cpu,bus,op&7,cpu->hl)); return (op&7)==6?7:4;
        case 0xB8:case 0xB9:case 0xBA:case 0xBB:case 0xBC:case 0xBD:case 0xBE:case 0xBF:
            do_cp (cpu,get_r(cpu,bus,op&7,cpu->hl)); return (op&7)==6?7:4;

        /* ALU immediate */
        case 0xC6: do_add(cpu,FETCH8(),false); return 7;
        case 0xCE: do_add(cpu,FETCH8(),(cpu->f&Z80_FLAG_C)!=0); return 7;
        case 0xD6: do_sub(cpu,FETCH8(),false); return 7;
        case 0xDE: do_sub(cpu,FETCH8(),(cpu->f&Z80_FLAG_C)!=0); return 7;
        case 0xE6: do_and(cpu,FETCH8()); return 7;
        case 0xEE: do_xor(cpu,FETCH8()); return 7;
        case 0xF6: do_or (cpu,FETCH8()); return 7;
        case 0xFE: do_cp (cpu,FETCH8()); return 7;

        /* PUSH / POP */
        case 0xC1: cpu->bc=pop16(cpu,bus); return 10;
        case 0xD1: cpu->de=pop16(cpu,bus); return 10;
        case 0xE1: cpu->hl=pop16(cpu,bus); return 10;
        case 0xF1: cpu->af=pop16(cpu,bus); return 10;
        case 0xC5: push16(cpu,bus,cpu->bc); return 11;
        case 0xD5: push16(cpu,bus,cpu->de); return 11;
        case 0xE5: push16(cpu,bus,cpu->hl); return 11;
        case 0xF5: push16(cpu,bus,cpu->af); return 11;

        /* JP absolute */
        case 0xC3: cpu->pc=FETCH16(); return 10;
        case 0xC2: { u16 a=FETCH16(); if(!(cpu->f&Z80_FLAG_Z)) cpu->pc=a; return 10; }
        case 0xCA: { u16 a=FETCH16(); if( (cpu->f&Z80_FLAG_Z)) cpu->pc=a; return 10; }
        case 0xD2: { u16 a=FETCH16(); if(!(cpu->f&Z80_FLAG_C)) cpu->pc=a; return 10; }
        case 0xDA: { u16 a=FETCH16(); if( (cpu->f&Z80_FLAG_C)) cpu->pc=a; return 10; }
        case 0xE2: { u16 a=FETCH16(); if(!(cpu->f&Z80_FLAG_PV))cpu->pc=a; return 10; }
        case 0xEA: { u16 a=FETCH16(); if( (cpu->f&Z80_FLAG_PV))cpu->pc=a; return 10; }
        case 0xF2: { u16 a=FETCH16(); if(!(cpu->f&Z80_FLAG_S)) cpu->pc=a; return 10; }
        case 0xFA: { u16 a=FETCH16(); if( (cpu->f&Z80_FLAG_S)) cpu->pc=a; return 10; }
        case 0xE9: cpu->pc=cpu->hl; return 4;

        /* CALL */
        case 0xCD: { u16 a=FETCH16(); push16(cpu,bus,cpu->pc); cpu->pc=a; return 17; }
        case 0xC4: { u16 a=FETCH16(); if(cc(cpu,0)){push16(cpu,bus,cpu->pc);cpu->pc=a;return 17;} return 10; }
        case 0xCC: { u16 a=FETCH16(); if(cc(cpu,1)){push16(cpu,bus,cpu->pc);cpu->pc=a;return 17;} return 10; }
        case 0xD4: { u16 a=FETCH16(); if(cc(cpu,2)){push16(cpu,bus,cpu->pc);cpu->pc=a;return 17;} return 10; }
        case 0xDC: { u16 a=FETCH16(); if(cc(cpu,3)){push16(cpu,bus,cpu->pc);cpu->pc=a;return 17;} return 10; }
        case 0xE4: { u16 a=FETCH16(); if(cc(cpu,4)){push16(cpu,bus,cpu->pc);cpu->pc=a;return 17;} return 10; }
        case 0xEC: { u16 a=FETCH16(); if(cc(cpu,5)){push16(cpu,bus,cpu->pc);cpu->pc=a;return 17;} return 10; }
        case 0xF4: { u16 a=FETCH16(); if(cc(cpu,6)){push16(cpu,bus,cpu->pc);cpu->pc=a;return 17;} return 10; }
        case 0xFC: { u16 a=FETCH16(); if(cc(cpu,7)){push16(cpu,bus,cpu->pc);cpu->pc=a;return 17;} return 10; }

        /* RET */
        case 0xC9: cpu->pc=pop16(cpu,bus); return 10;
        case 0xC0: { if(cc(cpu,0)){cpu->pc=pop16(cpu,bus);return 11;} return 5; }
        case 0xC8: { if(cc(cpu,1)){cpu->pc=pop16(cpu,bus);return 11;} return 5; }
        case 0xD0: { if(cc(cpu,2)){cpu->pc=pop16(cpu,bus);return 11;} return 5; }
        case 0xD8: { if(cc(cpu,3)){cpu->pc=pop16(cpu,bus);return 11;} return 5; }
        case 0xE0: { if(cc(cpu,4)){cpu->pc=pop16(cpu,bus);return 11;} return 5; }
        case 0xE8: { if(cc(cpu,5)){cpu->pc=pop16(cpu,bus);return 11;} return 5; }
        case 0xF0: { if(cc(cpu,6)){cpu->pc=pop16(cpu,bus);return 11;} return 5; }
        case 0xF8: { if(cc(cpu,7)){cpu->pc=pop16(cpu,bus);return 11;} return 5; }

        /* RST */
        case 0xC7: push16(cpu,bus,cpu->pc); cpu->pc=0x00; return 11;
        case 0xCF: push16(cpu,bus,cpu->pc); cpu->pc=0x08; return 11;
        case 0xD7:
            if (z80_rst10_hook) z80_rst10_hook(cpu, bus);
            push16(cpu,bus,cpu->pc); cpu->pc=0x10; return 11;
        case 0xDF: push16(cpu,bus,cpu->pc); cpu->pc=0x18; return 11;
        case 0xE7: push16(cpu,bus,cpu->pc); cpu->pc=0x20; return 11;
        case 0xEF: push16(cpu,bus,cpu->pc); cpu->pc=0x28; return 11;
        case 0xF7: push16(cpu,bus,cpu->pc); cpu->pc=0x30; return 11;
        case 0xFF: push16(cpu,bus,cpu->pc); cpu->pc=0x38; return 11;

        /* EXX / EX DE,HL / EX (SP),HL / LD SP,HL */
        case 0xD9: { u16 t; t=cpu->bc;cpu->bc=cpu->bc_;cpu->bc_=t; t=cpu->de;cpu->de=cpu->de_;cpu->de_=t; t=cpu->hl;cpu->hl=cpu->hl_;cpu->hl_=t; return 4; }
        case 0xEB: { u16 t=cpu->de; cpu->de=cpu->hl; cpu->hl=t; return 4; }
        case 0xE3: { u16 t=read16(bus,cpu->sp); write16(bus,cpu->sp,cpu->hl); cpu->hl=t; return 19; }
        case 0xF9: cpu->sp=cpu->hl; return 6;

        /* DI / EI */
        case 0xF3: cpu->iff1=cpu->iff2=false; return 4;
        case 0xFB: cpu->iff1=cpu->iff2=true; cpu->ei_delay=true; return 4;

        /* IN / OUT — split-cycle accounting via Z80Bus::tick when available.
         * konCePCja runs the pre-IO cycles through z80_wait_states BEFORE
         * the actual IO operation completes (z80.cpp:1357/1479), then the
         * remaining cycles tick after. We mirror that by tick'ing N cycles
         * pre-IO (Ia=12 / Oa=12 minus the trailing 0/4 cycles); the
         * post-IO remainder is processed by cpc_frame's usual per-step
         * advance. NULL tick = legacy single-chunk behavior. */
        case 0xDB: {
            u8 n = FETCH8();
            /* IN A,(n) is 12 cycles total. Pre-ticking 12 cycles before
             * the IN regressed sweep to 3/13, so kept as single-chunk. */
            cpu->a = IN((cpu->a<<8)|n);
            return 11;
        }
        case 0xD3: {
            u8 n = FETCH8();
            /* konCePCja split for OUT (n),A is Oa=8 pre-IO + Oa_=4 post-IO
             * (z80.cpp:1479). Pre-tick 8 cycles so the bus / GA state at
             * the OUT write matches the reference; the remaining 4 ticks
             * after via cpc_frame's per-step advance (total cc_op[0xD3]=12). */
            if (bus->tick) bus->tick(bus->ctx, 8);
            OUT((cpu->a<<8)|n, cpu->a);
            return 11;
        }

        /* Prefixes */
        case 0xCB: cpu->last_prefix = 0xCB; return exec_cb(cpu, bus);
        case 0xED: cpu->last_prefix = 0xED; return exec_ed(cpu, bus);
        case 0xDD: cpu->last_prefix = 0xDD; return exec_xy(cpu, bus, &cpu->ix);
        case 0xFD: cpu->last_prefix = 0xFD; return exec_xy(cpu, bus, &cpu->iy);

        default:
            fprintf(stderr, "Unimplemented opcode: 0x%02X at PC=0x%04X\n", op, cpu->pc - 1);
            return 4;
    }
}
