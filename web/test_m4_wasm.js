"use strict";

/* Headless WASM test for the M4 expansion exports: board enable/disable,
 * FAT SD image mounting, and stability while stepping.
 * Run with: node test_m4_wasm.js   (needs a built dist/) */

const assert = require("node:assert/strict");
const create6128 = require("./dist/6128.js");

// Minimal 1.44 MB FAT12 image with an empty root directory. FatFs should
// recognise and mount it, giving the M4 a working sector/FAT volume.
function fat12Image() {
  const image = new Uint8Array(2880 * 512);
  const b = new DataView(image.buffer);
  const ascii = (offset, text) => {
    for (let i = 0; i < text.length; i++) image[offset + i] = text.charCodeAt(i);
  };
  image[0] = 0xEB; image[1] = 0x3C; image[2] = 0x90;
  ascii(3, "JS1984  ");
  b.setUint16(11, 512, true);   // bytes per sector
  b.setUint8(13, 1);            // sectors per cluster
  b.setUint16(14, 1, true);     // reserved sectors
  b.setUint8(15, 2);            // number of FATs
  b.setUint16(16, 224, true);   // root entries
  b.setUint16(18, 2880, true);  // total sectors (16-bit)
  b.setUint8(20, 0xF0);         // media descriptor
  b.setUint16(21, 9, true);     // FAT size
  b.setUint16(23, 18, true);    // sectors per track
  b.setUint16(25, 2, true);     // heads
  b.setUint16(27, 0, true);     // hidden sectors
  b.setUint16(32, 0, true);     // drive number
  b.setUint8(34, 0x29);         // extended boot signature
  b.setUint32(35, 0x19841984, true); // volume serial
  ascii(39, "JS1984 SD   ");
  ascii(50, "FAT12   ");
  image[510] = 0x55; image[511] = 0xAA;
  // FAT1 (sector 1): entries 0/1 = 0xFF0 / 0xFFF, rest free.
  const fat = 1 * 512;
  image[fat] = 0xF0; image[fat + 1] = 0xFF; image[fat + 2] = 0xFF;
  // FAT2 mirrors FAT1.
  const fat2 = 10 * 512;
  image.set(image.subarray(fat, fat + 512), fat2);
  return image;
}

create6128().then(m => {
  if (m._poc_init() !== 0) throw new Error("init failed");

  assert.equal(m._poc_m4_enabled(), 0, "M4 disabled by default");

  // Installing the board firmware and enabling the MX4 bus must succeed.
  assert.equal(m._poc_set_m4(1), 0, "M4 board enabled");
  assert.equal(m._poc_m4_enabled(), 1, "M4 now enabled");
  for (let i = 0; i < 2000; i++) m._poc_step();

  // Mount a FAT12 image written into the virtual filesystem.
  m.FS.writeFile("/m4.img", fat12Image());
  assert.equal(
    m.ccall("poc_mount_m4_sd", "number", ["string"], ["/m4.img"]),
    0,
    "SD image mounted"
  );
  for (let i = 0; i < 2000; i++) m._poc_step();

  // A garbage image must be rejected (no valid FAT volume).
  m.FS.writeFile("/bad.img", new Uint8Array(2880 * 512));
  assert.notEqual(
    m.ccall("poc_mount_m4_sd", "number", ["string"], ["/bad.img"]),
    0,
    "garbage SD image rejected"
  );

  m._poc_eject_m4_sd();
  assert.equal(m._poc_set_m4(0), 0, "M4 board disabled");
  assert.equal(m._poc_m4_enabled(), 0, "M4 disabled again");
  for (let i = 0; i < 2000; i++) m._poc_step();

  console.log("M4 WASM tests passed");
}).catch(error => {
  console.error(error);
  process.exit(1);
});
