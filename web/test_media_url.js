'use strict';

const assert = require('assert');
const { parseStartupMedia, filenameFromUrl } = require('./media-url.js');

const base = 'https://example.test/1984/';

let media = parseStartupMedia(
  '?theme=sapporo-dark&memory=512&diska=media%2Fsystem.dsk&diskb=media%2Fdata.dsk&autorun=disc.bas',
  base
);
assert.deepStrictEqual(media, {
  diskA: 'https://example.test/1984/media/system.dsk',
  diskB: 'https://example.test/1984/media/data.dsk',
  cartridge: null,
  autorun: 'disc.bas',
  memoryKb: 512,
});

media = parseStartupMedia(
  '?cartridge=https%3A%2F%2Fcdn.example.test%2Fgames%2FSonic.cpr',
  base
);
assert.strictEqual(media.diskA, null);
assert.strictEqual(media.diskB, null);
assert.strictEqual(media.cartridge, 'https://cdn.example.test/games/Sonic.cpr');
assert.strictEqual(media.autorun, null);
assert.strictEqual(media.memoryKb, null);

assert.strictEqual(
  filenameFromUrl('https://example.test/media/Bomb%20Jack.dsk', 'disk.dsk'),
  'Bomb Jack.dsk'
);
assert.deepStrictEqual(parseStartupMedia('?theme=default', base), {
  diskA: null,
  diskB: null,
  cartridge: null,
  autorun: null,
  memoryKb: null,
});

for (const memoryKb of [128, 256, 512, 1024]) {
  media = parseStartupMedia('?memory=' + memoryKb, base);
  assert.strictEqual(media.memoryKb, memoryKb);
}

/* Keep old published links functional while documenting diska as canonical. */
media = parseStartupMedia('?disk=legacy.dsk', base);
assert.strictEqual(media.diskA, 'https://example.test/1984/legacy.dsk');
assert.strictEqual(media.diskB, null);

media = parseStartupMedia('?disk=legacy.dsk&diska=current.dsk', base);
assert.strictEqual(media.diskA, 'https://example.test/1984/current.dsk');

assert.throws(
  () => parseStartupMedia('?diskb=file%3A%2F%2F%2Ftmp%2Fprivate.dsk', base),
  /HTTP or HTTPS/
);
assert.throws(
  () => parseStartupMedia('?autorun=disc.bas', base),
  /requires a diska/
);
assert.throws(
  () => parseStartupMedia('?diskb=data.dsk&autorun=disc.bas', base),
  /requires a diska/
);
assert.throws(
  () => parseStartupMedia('?diska=game.dsk&autorun=bad%22name', base),
  /unsupported characters/
);
assert.throws(
  () => parseStartupMedia('?memory=64', base),
  /memory must be 128, 256, 512, or 1024/
);
assert.throws(
  () => parseStartupMedia('?memory=512KB', base),
  /memory must be 128, 256, 512, or 1024/
);

console.log('server media URL tests passed');
