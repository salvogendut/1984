"use strict";

/* Headless WASM test for the ROM-slot exports: load/unload/present, invalid
 * slots, and the 128-byte AMSDOS header skip.
 * Run with: node test_rom_slots_wasm.js   (needs a built dist/) */

const assert = require("node:assert/strict");
const create6128 = require("./dist/6128.js");

function romImage(size = 16384) {
  const rom = new Uint8Array(size);
  for (let i = 0; i < size; i++) rom[i] = (i * 7) & 0xff;
  return rom;
}

create6128().then(m => {
  if (m._poc_init() !== 0) throw new Error("init failed");

  // Nothing loaded at boot (AMSDOS is the slot-7 fallback, not rom_ext).
  assert.equal(m._poc_rom_slot_present(5), 0, "slot 5 empty by default");
  assert.equal(m._poc_rom_slot_present(7), 0, "AMSDOS fallback not in rom_ext");

  // Plain 16 KB image.
  m.FS.writeFile("/test.rom", romImage());
  assert.equal(
    m.ccall("poc_rom_slot_load", "number", ["number", "string"], [5, "/test.rom"]),
    0, "16 KB ROM loaded"
  );
  assert.equal(m._poc_rom_slot_present(5), 1, "slot 5 now present");
  for (let i = 0; i < 2000; i++) m._poc_step();

  // 16 KB + 128-byte AMSDOS header is skipped transparently.
  m.FS.writeFile("/hd.rom", romImage(16384 + 128));
  assert.equal(
    m.ccall("poc_rom_slot_load", "number", ["number", "string"], [4, "/hd.rom"]),
    0, "header-padded ROM loaded"
  );
  assert.equal(m._poc_rom_slot_present(4), 1, "slot 4 now present");

  // Invalid slot and missing file must be rejected.
  assert.notEqual(
    m.ccall("poc_rom_slot_load", "number", ["number", "string"], [99, "/test.rom"]),
    0, "slot 99 rejected"
  );
  assert.notEqual(
    m.ccall("poc_rom_slot_load", "number", ["number", "string"], [3, "/nope.rom"]),
    0, "missing file rejected"
  );

  // Unloading clears the presence flag.
  m._poc_rom_slot_unload(5);
  assert.equal(m._poc_rom_slot_present(5), 0, "slot 5 unloaded");
  for (let i = 0; i < 2000; i++) m._poc_step();

  console.log("ROM slots WASM tests passed");
}).catch(error => {
  console.error(error);
  process.exit(1);
});
