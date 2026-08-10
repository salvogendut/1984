'use strict';

const assert = require('assert');
const { mapGamepad, selectProfile } = require('./gamepad.js');

function buttons(count, pressed) {
  const active = new Set(pressed);
  return Array.from({ length: count }, (_, i) => ({
    pressed: active.has(i),
    value: active.has(i) ? 1 : 0,
  }));
}

function pad(options) {
  return {
    id: options.id || 'test pad',
    mapping: options.mapping || '',
    buttons: buttons(options.buttonCount || 17, options.pressed || []),
    axes: options.axes || [0, 0, 0, 0],
  };
}

let gamepad = pad({ mapping: 'standard', pressed: [12, 0] });
assert.strictEqual(selectProfile(gamepad), 'standard');
assert.deepStrictEqual(mapGamepad(gamepad).state, [1, 0, 0, 0, 1, 0]);

gamepad = pad({ mapping: 'standard', axes: [-0.8, 0.9, 0, 0] });
assert.deepStrictEqual(mapGamepad(gamepad).state, [0, 1, 1, 0, 0, 0]);

gamepad = pad({
  id: '054c-0268-Sony PLAYSTATION(R)3 Controller',
  pressed: [0, 13, 16],
  axes: [0, 0, 0, 0, 0, 0],
});
assert.strictEqual(selectProfile(gamepad), 'ps3-raw');
assert.deepStrictEqual(mapGamepad(gamepad).state, [1, 0, 0, 1, 1, 0]);

const legacyAxes = Array(14).fill(-1);
legacyAxes[0] = 0;
legacyAxes[1] = 0;
legacyAxes[8] = 1;
legacyAxes[9] = 1;
gamepad = pad({
  id: 'Sony PLAYSTATION(R)3 Controller',
  pressed: [14],
  axes: legacyAxes,
});
assert.strictEqual(selectProfile(gamepad), 'ps3-legacy-raw');
assert.deepStrictEqual(mapGamepad(gamepad).state, [1, 0, 0, 1, 1, 0]);

gamepad = pad({
  id: 'Generic USB joystick',
  pressed: [2],
  axes: [0, 0, 0, 0, 0, 0, -1, 1],
});
assert.strictEqual(selectProfile(gamepad), 'generic-raw');
assert.deepStrictEqual(mapGamepad(gamepad).state, [0, 1, 1, 0, 0, 1]);

console.log('gamepad mapping tests passed');
