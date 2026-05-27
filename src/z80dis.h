#pragma once
#include "types.h"
#include <stddef.h>

/* Disassemble one Z80 instruction.
 * mem:   65536-byte flat (CPU-visible) memory snapshot
 * pc:    address of the instruction
 * out:   output buffer  (e.g. "LD HL,0B900h")
 * outsz: size of output buffer
 * Returns number of bytes consumed. */
int z80dis(const u8 *mem, u16 pc, char *out, size_t outsz);
