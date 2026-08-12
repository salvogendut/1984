"use strict";

const assert = require("assert");
const monitor = require("./ml-monitor.js");

assert.strictEqual(monitor.parseAddress("4000"), 0x4000);
assert.strictEqual(monitor.parseAddress("0xBEEF"), 0xbeef);
assert.strictEqual(monitor.parseAddress("&00ff"), 0xff);
assert.throws(() => monitor.parseAddress("10000"), /0000-FFFF/);
assert.throws(() => monitor.parseAddress("xyz"), /0000-FFFF/);

assert.strictEqual(monitor.parseLength("32"), 32);
assert.throws(() => monitor.parseLength("0"), /1-256/);
assert.throws(() => monitor.parseLength("257"), /1-256/);

assert.deepStrictEqual(monitor.parseBytes("00 ff,7A 0x10 &20"),
                       [0x00, 0xff, 0x7a, 0x10, 0x20]);
assert.throws(() => monitor.parseBytes("GG"), /invalid byte/);
assert.throws(() => monitor.parseBytes(""), /at least one/);

assert.strictEqual(
  monitor.formatMemory(0xfffc, [0x41, 0x00, 0x7e, 0x20, 0x42]),
  "FFFC  41 00 7E 20 42           A.~ B"
);
assert.strictEqual(monitor.normalizeLabel("  player_x  "), "player_x");
assert.throws(() => monitor.normalizeLabel(""), /required/);

console.log("ML monitor utility tests passed");
