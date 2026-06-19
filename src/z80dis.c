#include "z80dis.h"
#include <stdio.h>
#include <string.h>

static const char * const r8[8]  = {"B","C","D","E","H","L","(HL)","A"};
static const char * const rp[4]  = {"BC","DE","HL","SP"};
static const char * const rp2[4] = {"BC","DE","HL","AF"};
static const char * const cc[8]  = {"NZ","Z","NC","C","PO","PE","P","M"};
static const char * const alu[8] = {"ADD A,","ADC A,","SUB ","SBC A,","AND ","XOR ","OR ","CP "};
static const char * const rot[8] = {"RLC","RRC","RL","RR","SLA","SRA","SLL","SRL"};

static u8  rb(const u8 *m, u16 *p) { u8 v = m[*p]; (*p)++; return v; }
static u16 rw(const u8 *m, u16 *p) { u8 lo=rb(m,p); return (u16)(lo | (rb(m,p)<<8)); }

/* Format (IX/IY+d) or (IX/IY-d) into tmp (>=16 bytes), return pointer to it. */
static const char *idxstr(const char *xy, s8 d, char *tmp) {
    if (d >= 0) snprintf(tmp, 16, "(%s+%02Xh)", xy, (u8)d);
    else        snprintf(tmp, 16, "(%s-%02Xh)", xy, (u8)(-d));
    return tmp;
}

/* r8 name with optional IX/IY substitution.
 * xy="IX"/"IY"/NULL; d=displacement (only used when r==6 and xy!=NULL) */
static const char *rn(int r, const char *xy, s8 d, char *tmp) {
    if (xy == NULL) return r8[r];
    if (r == 6)     return idxstr(xy, d, tmp);
    if (r == 4)     { snprintf(tmp, 16, "%sH", xy); return tmp; }
    if (r == 5)     { snprintf(tmp, 16, "%sL", xy); return tmp; }
    return r8[r];
}

/* CB-prefixed opcodes (rot/bit) — xy!=NULL means DDCB/FDCB form */
static int dis_cb(const u8 *m, u16 *pc, char *out, size_t sz,
                  const char *xy, s8 disp) {
    u8 op = rb(m, pc);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    char tmp[16];
    const char *dst = xy ? idxstr(xy, disp, tmp) : r8[z];

    if (x == 0) {
        if (xy) snprintf(out, sz, "%s %s", rot[y], dst);
        else    snprintf(out, sz, "%s %s", rot[y], dst);
    } else if (x == 1) {
        snprintf(out, sz, "BIT %d,%s", y, dst);
    } else if (x == 2) {
        snprintf(out, sz, "RES %d,%s", y, dst);
    } else {
        snprintf(out, sz, "SET %d,%s", y, dst);
    }
    return 0; /* caller tracks length */
}

/* ED-prefixed opcodes */
static int dis_ed(const u8 *m, u16 *pc, char *out, size_t sz) {
    u8 op = rb(m, pc);
    int y = (op >> 3) & 7, z = op & 7;
    int p = y >> 1, q = y & 1;

    if (op >= 0x40 && op <= 0x7F) {
        switch (z) {
        case 0: snprintf(out, sz, "IN %s,(C)", r8[y]); break;
        case 1:
            if (y == 6) snprintf(out, sz, "OUT (C),0");
            else        snprintf(out, sz, "OUT (C),%s", r8[y]);
            break;
        case 2:
            if (q) snprintf(out, sz, "ADC HL,%s", rp[p]);
            else   snprintf(out, sz, "SBC HL,%s", rp[p]);
            break;
        case 3: {
            u16 nn = rw(m, pc);
            if (q) snprintf(out, sz, "LD %s,(%04Xh)", rp[p], nn);
            else   snprintf(out, sz, "LD (%04Xh),%s", nn, rp[p]);
            break;
        }
        case 4: snprintf(out, sz, "NEG"); break;
        case 5: snprintf(out, sz, (op == 0x4D) ? "RETI" : "RETN"); break;
        case 6: {
            static const char * const im_t[4] = {"0","0/1","1","2"};
            snprintf(out, sz, "IM %s", im_t[y & 3]);
            break;
        }
        case 7:
            switch (y) {
            case 0: snprintf(out, sz, "LD I,A");   break;
            case 1: snprintf(out, sz, "LD R,A");   break;
            case 2: snprintf(out, sz, "LD A,I");   break;
            case 3: snprintf(out, sz, "LD A,R");   break;
            case 4: snprintf(out, sz, "RRD");      break;
            case 5: snprintf(out, sz, "RLD");      break;
            default: snprintf(out, sz, "NOP");     break;
            }
            break;
        }
    } else if (op >= 0xA0 && op <= 0xBF) {
        static const char * const bli[4][4] = {
            {"LDI","CPI","INI","OUTI"},
            {"LDD","CPD","IND","OUTD"},
            {"LDIR","CPIR","INIR","OTIR"},
            {"LDDR","CPDR","INDR","OTDR"},
        };
        int row = (op >> 4) - 0xA, col = op & 3;
        if (row < 4 && col < 4) snprintf(out, sz, "%s", bli[row][col]);
        else snprintf(out, sz, "NOP");
    } else {
        snprintf(out, sz, "NOP");
    }
    return 0;
}

/* Main decoder — handles no-prefix, DD, and FD.
 * xy = "IX" / "IY" / NULL */
static int dis_main(const u8 *m, u16 *pc, char *out, size_t sz, const char *xy) {
    char tmp1[16], tmp2[16];
    u8 op = rb(m, pc);

    /* Prefixes that can occur inside DD/FD space */
    if (op == 0xCB) {
        s8 disp = 0;
        if (xy) disp = (s8)rb(m, pc);  /* DDCB/FDCB: displacement before opcode */
        dis_cb(m, pc, out, sz, xy, disp);
        return 0;
    }
    if (op == 0xED) { return dis_ed(m, pc, out, sz); }
    if (op == 0xDD) { return dis_main(m, pc, out, sz, "IX"); }
    if (op == 0xFD) { return dis_main(m, pc, out, sz, "IY"); }

    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    int p = y >> 1, q = y & 1;

    /* For DD/FD prefix, instructions that read r8[y] or r8[z] where those
     * equal 6 need to consume a displacement byte. We read it lazily. */
    s8 disp = 0;
    bool disp_read = false;
    #define NEED_DISP() do { if (xy && !disp_read) { disp=(s8)rb(m,pc); disp_read=true; } } while(0)

    /* HL register name (16-bit) */
    const char *HL = xy ? xy : "HL";

    if (x == 1) {
        /* LD r,r' — 0x76 is HALT */
        if (y == 6 && z == 6) { snprintf(out, sz, "HALT"); return 0; }
        bool yneed = (y == 6), zneed = (z == 6);
        if ((yneed || zneed) && xy) { disp=(s8)rb(m,pc); disp_read=true; }
        snprintf(out, sz, "LD %s,%s", rn(y, yneed?xy:NULL, disp, tmp1),
                                       rn(z, zneed?xy:NULL, disp, tmp2));
        return 0;
    }

    if (x == 2) {
        if (z == 6 && xy) { NEED_DISP(); }
        snprintf(out, sz, "%s%s", alu[y], rn(z, (z==6)?xy:NULL, disp, tmp1));
        return 0;
    }

    if (x == 0) {
        switch (z) {
        case 0:
            switch (y) {
            case 0: snprintf(out, sz, "NOP"); break;
            case 1: snprintf(out, sz, "EX AF,AF'"); break;
            case 2: { s8 d=(s8)rb(m,pc); snprintf(out, sz, "DJNZ %04Xh", (u16)(*pc+d)); break; }
            case 3: { s8 d=(s8)rb(m,pc); snprintf(out, sz, "JR %04Xh",   (u16)(*pc+d)); break; }
            default:{ s8 d=(s8)rb(m,pc); snprintf(out, sz, "JR %s,%04Xh", cc[y-4], (u16)(*pc+d)); break; }
            }
            break;
        case 1:
            if (q == 0) { u16 nn=rw(m,pc); snprintf(out, sz, "LD %s,%04Xh", (p==2)?HL:rp[p], nn); }
            else        snprintf(out, sz, "ADD %s,%s", HL, (p==2)?HL:rp[p]);
            break;
        case 2:
            switch (y) {
            case 0: snprintf(out, sz, "LD (BC),A"); break;
            case 1: snprintf(out, sz, "LD A,(BC)"); break;
            case 2: snprintf(out, sz, "LD (DE),A"); break;
            case 3: snprintf(out, sz, "LD A,(DE)"); break;
            case 4: { u16 nn=rw(m,pc); snprintf(out, sz, "LD (%04Xh),%s", nn, HL); break; }
            case 5: { u16 nn=rw(m,pc); snprintf(out, sz, "LD %s,(%04Xh)", HL, nn); break; }
            case 6: { u16 nn=rw(m,pc); snprintf(out, sz, "LD (%04Xh),A",  nn); break; }
            case 7: { u16 nn=rw(m,pc); snprintf(out, sz, "LD A,(%04Xh)",  nn); break; }
            }
            break;
        case 3:
            if (q == 0) snprintf(out, sz, "INC %s", (p==2)?HL:rp[p]);
            else        snprintf(out, sz, "DEC %s", (p==2)?HL:rp[p]);
            break;
        case 4:
            if (y == 6 && xy) { NEED_DISP(); }
            snprintf(out, sz, "INC %s", rn(y, (y==6)?xy:NULL, disp, tmp1));
            break;
        case 5:
            if (y == 6 && xy) { NEED_DISP(); }
            snprintf(out, sz, "DEC %s", rn(y, (y==6)?xy:NULL, disp, tmp1));
            break;
        case 6:
            if (y == 6 && xy) { NEED_DISP(); }
            { u8 n=rb(m,pc); snprintf(out, sz, "LD %s,%02Xh", rn(y,(y==6)?xy:NULL,disp,tmp1), n); }
            break;
        case 7:
            switch (y) {
            case 0: snprintf(out, sz, "RLCA"); break;
            case 1: snprintf(out, sz, "RRCA"); break;
            case 2: snprintf(out, sz, "RLA");  break;
            case 3: snprintf(out, sz, "RRA");  break;
            case 4: snprintf(out, sz, "DAA");  break;
            case 5: snprintf(out, sz, "CPL");  break;
            case 6: snprintf(out, sz, "SCF");  break;
            case 7: snprintf(out, sz, "CCF");  break;
            }
            break;
        }
        return 0;
    }

    /* x == 3 */
    switch (z) {
    case 0: snprintf(out, sz, "RET %s", cc[y]); break;
    case 1:
        if (q == 0) snprintf(out, sz, "POP %s", (p==2)?HL:rp2[p]);
        else switch (p) {
            case 0: snprintf(out, sz, "RET");         break;
            case 1: snprintf(out, sz, "EXX");         break;
            case 2: snprintf(out, sz, "JP (%s)", HL); break;
            case 3: snprintf(out, sz, "LD SP,%s", HL);break;
        }
        break;
    case 2: { u16 nn=rw(m,pc); snprintf(out, sz, "JP %s,%04Xh", cc[y], nn); break; }
    case 3:
        switch (y) {
        case 0: { u16 nn=rw(m,pc); snprintf(out, sz, "JP %04Xh", nn); break; }
        case 1: snprintf(out, sz, "(CB prefix)"); break;  /* shouldn't reach here */
        case 2: { u8 n=rb(m,pc); snprintf(out, sz, "OUT (%02Xh),A", n); break; }
        case 3: { u8 n=rb(m,pc); snprintf(out, sz, "IN A,(%02Xh)",  n); break; }
        case 4: snprintf(out, sz, "EX (SP),%s", HL); break;
        case 5: snprintf(out, sz, "EX DE,HL");  break;
        case 6: snprintf(out, sz, "DI");        break;
        case 7: snprintf(out, sz, "EI");        break;
        }
        break;
    case 4: { u16 nn=rw(m,pc); snprintf(out, sz, "CALL %s,%04Xh", cc[y], nn); break; }
    case 5:
        if (q == 0) snprintf(out, sz, "PUSH %s", (p==2)?HL:rp2[p]);
        else switch (p) {
            case 0: { u16 nn=rw(m,pc); snprintf(out, sz, "CALL %04Xh", nn); break; }
            default: snprintf(out, sz, "NOP"); break; /* DD/ED/FD handled above */
        }
        break;
    case 6: { u8 n=rb(m,pc); snprintf(out, sz, "%s%02Xh", alu[y], n); break; }
    case 7: snprintf(out, sz, "RST %02Xh", y * 8); break;
    }
    #undef NEED_DISP
    return 0;
}

int z80dis(const u8 *mem, u16 pc, char *out, size_t outsz) {
    u16 start = pc;
    dis_main(mem, &pc, out, outsz, NULL);
    return (int)(u16)(pc - start);
}
