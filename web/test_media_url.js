'use strict';

const assert = require('assert');
const { parseStartupMedia, filenameFromUrl } = require('./media-url.js');

const base = 'https://example.test/1984/';

let media = parseStartupMedia(
  '?theme=sapporo-dark&disk=media%2Fthisdisk.dsk&autorun=disc.bas',
  base
);
assert.deepStrictEqual(media, {
  disk: 'https://example.test/1984/media/thisdisk.dsk',
  cartridge: null,
  autorun: 'disc.bas',
});

media = parseStartupMedia(
  '?cartridge=https%3A%2F%2Fcdn.example.test%2Fgames%2FSonic.cpr',
  base
);
assert.strictEqual(media.disk, null);
assert.strictEqual(media.cartridge, 'https://cdn.example.test/games/Sonic.cpr');
assert.strictEqual(media.autorun, null);

assert.strictEqual(
  filenameFromUrl('https://example.test/media/Bomb%20Jack.dsk', 'disk.dsk'),
  'Bomb Jack.dsk'
);
assert.deepStrictEqual(parseStartupMedia('?theme=default', base), {
  disk: null,
  cartridge: null,
  autorun: null,
});

assert.throws(
  () => parseStartupMedia('?disk=file%3A%2F%2F%2Ftmp%2Fprivate.dsk', base),
  /HTTP or HTTPS/
);
assert.throws(
  () => parseStartupMedia('?autorun=disc.bas', base),
  /requires a disk/
);
assert.throws(
  () => parseStartupMedia('?disk=game.dsk&autorun=bad%22name', base),
  /unsupported characters/
);

console.log('server media URL tests passed');
