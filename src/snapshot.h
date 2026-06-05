#pragma once
#include "cpc.h"

/* SNA (Amstrad CPC snapshot) v1-v3 loader.
 *
 * Format reference: https://www.cpcwiki.eu/index.php/Snapshot
 *   - 256-byte header at offset 0 ("MV - SNA" signature).
 *   - Z80 registers, GA state, CRTC regs, PPI, PSG, upper-ROM-select.
 *   - Memory dump starts at offset 0x100 (header[0x6B-0x6C] = size in KB,
 *     typically 64 or 128 — extension banks beyond 64 KB main appear in
 *     order: bank 4, 5, 6, 7).
 *
 * Returns 0 on success, negative on error (with a diagnostic on stderr).
 */
int snapshot_load(CPC *cpc, const char *path);

/* Write the current CPU/GA/CRTC/PPI/PSG/RAM state to a v3 SNA file. The
 * RAM-size field is set from cpc->mem.ram_size (rounded down to KB) and
 * exactly that many bytes are written after the header. Returns 0 on success. */
int snapshot_save(CPC *cpc, const char *path);
