"use strict";

/* Headless WASM test for PowerGraph enable/disable, output selection, and
 * persistence across browser-side machine changes.
 * Run with: node test_powergraph_wasm.js   (needs a built dist/) */

const assert = require("node:assert/strict");
const create6128 = require("./dist/6128.js");

create6128().then(m => {
  if (m._poc_init() !== 0) throw new Error("init failed");

  assert.equal(m._poc_powergraph_v9990_enabled(), 0,
               "PowerGraph disabled by default");
  assert.equal(m._poc_powergraph_video_source(), 0,
               "automatic output selected by default");
  assert.equal(m._poc_powergraph_output_active(), 0,
               "automatic output initially keeps the CPC display");
  assert.equal(m._poc_set_powergraph_video_source(3), -1,
               "invalid output source rejected");

  assert.equal(m._poc_set_powergraph_v9990(1), 1,
               "PowerGraph enabled");
  assert.equal(m._poc_powergraph_v9990_enabled(), 1,
               "PowerGraph reports enabled");
  assert.equal(m._poc_powergraph_output_active(), 0,
               "Auto waits for V9990 display enable");

  assert.equal(m._poc_set_powergraph_video_source(2), 2,
               "V9990 output can be forced");
  assert.equal(m._poc_powergraph_output_active(), 1,
               "forced V9990 output is active");
  for (let i = 0; i < 120; i++) m._poc_step();

  assert.equal(m._poc_init_model(1, 0), 0,
               "machine can change while PowerGraph is fitted");
  assert.equal(m._poc_powergraph_v9990_enabled(), 1,
               "PowerGraph survives a machine change");
  assert.equal(m._poc_powergraph_video_source(), 2,
               "output selection survives a machine change");
  assert.equal(m._poc_powergraph_output_active(), 1,
               "forced output remains active after a machine change");

  assert.equal(m._poc_set_powergraph_video_source(1), 1,
               "CPC output can be forced");
  assert.equal(m._poc_powergraph_output_active(), 0,
               "CPC output override is active");
  assert.equal(m._poc_set_powergraph_v9990(0), 0,
               "PowerGraph disabled");
  assert.equal(m._poc_powergraph_v9990_enabled(), 0,
               "PowerGraph reports disabled again");

  console.log("PowerGraph V9990 WASM tests passed");
}).catch(error => {
  console.error(error);
  process.exit(1);
});
