"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const DAP = require("./dap.js");

function ok(session, command, args) {
  const response = session.request(command, args || {});
  assert.strictEqual(response.success, true,
    response.body && response.body.error ? response.body.error.format : response.message);
  return response.body || {};
}

async function main() {
  const dist = path.join(__dirname, "dist");
  const source = fs.readFileSync(path.join(dist, "6128.js"), "utf8");
  const create6128 = new Function(
    "require", "__filename", "__dirname", source + "\nreturn create6128;"
  )(require, path.join(dist, "6128.js"), dist);
  const module = await create6128({
    wasmBinary: fs.readFileSync(path.join(dist, "6128.wasm")),
    print: () => {},
    printErr: () => {},
  });
  assert.strictEqual(module._poc_init(), 0);

  const session = new DAP.Session({
    isPaused: () => Boolean(module._poc_debug_is_paused()),
    pause: () => module._poc_debug_pause(),
    continue: () => module._poc_debug_continue(),
    stepIn: () => module._poc_debug_step_in(),
    next: () => module._poc_debug_next(),
    stepOut: () => module._poc_debug_step_out(),
    stepBack: () => module._poc_debug_step_back(),
    canStepBack: () => Boolean(module._poc_debug_can_step_back()),
    stopReason: () => module._poc_debug_stop_reason(),
    register: index => module._poc_debug_reg(index),
    setBreakpoint: address => module._poc_debug_breakpoint_set(address),
    clearBreakpoint: slot => module._poc_debug_breakpoint_clear(slot),
    readMemory: address => module._poc_debug_mem_read(address) & 0xff,
    writeMemory: (address, value) => module._poc_debug_mem_write_byte(address, value),
    disassemble: (address, count) => module.ccall(
      "poc_debug_disassemble", "string", ["number", "number"], [address, count]
    ),
  });

  const capabilities = ok(session, "initialize", {
    adapterID: "1984-z80",
    linesStartAt1: true,
    columnsStartAt1: true,
    supportsMemoryReferences: true,
    supportsMemoryEvent: true,
  });
  assert.strictEqual(capabilities.supportsInstructionBreakpoints, true);
  ok(session, "attach");
  assert.strictEqual(session.takeEvents()[0].event, "initialized");
  ok(session, "configurationDone");

  ok(session, "pause", { threadId: DAP.THREAD_ID });
  assert.strictEqual(session.takeEvents()[0].event, "stopped");
  const before = ok(session, "stackTrace", { threadId: DAP.THREAD_ID });
  const frame = before.stackFrames[0];
  const scopes = ok(session, "scopes", { frameId: frame.id });
  const variables = ok(session, "variables", {
    variablesReference: scopes.scopes[0].variablesReference,
  });
  assert.strictEqual(variables.variables.length, 14);
  const originalPc = DAP.parseReference(frame.instructionPointerReference);

  const disassembly = ok(session, "disassemble", {
    memoryReference: frame.instructionPointerReference,
    instructionCount: 8,
  });
  assert.strictEqual(disassembly.instructions.length, 8);
  assert.strictEqual(disassembly.instructions[0].address, DAP.addressReference(originalPc));

  ok(session, "stepIn", { threadId: DAP.THREAD_ID, granularity: "instruction" });
  module._poc_step();
  session.sync();
  assert.strictEqual(session.takeEvents()[0].body.reason, "step");
  assert.strictEqual(module._poc_debug_can_step_back(), 1);
  ok(session, "stepBack", { threadId: DAP.THREAD_ID, granularity: "instruction" });
  session.takeEvents();
  assert.strictEqual(module._poc_debug_reg(7), originalPc);

  ok(session, "next", { threadId: DAP.THREAD_ID, granularity: "instruction" });
  for (let frameCount = 0; frameCount < 100 && !module._poc_debug_is_paused(); frameCount++)
    module._poc_step();
  session.sync();
  assert.strictEqual(module._poc_debug_is_paused(), 1);
  assert.strictEqual(session.takeEvents()[0].body.reason, "step");

  const address = 0x4000;
  const memory = ok(session, "readMemory", {
    memoryReference: DAP.addressReference(address),
    count: 4,
  });
  const original = DAP.decodeBase64(memory.data);
  const replacement = Uint8Array.from(original, value => value ^ 0xff);
  ok(session, "writeMemory", {
    memoryReference: DAP.addressReference(address),
    data: DAP.encodeBase64(replacement),
  });
  session.takeEvents();
  const changed = ok(session, "readMemory", {
    memoryReference: DAP.addressReference(address),
    count: 4,
  });
  assert.deepStrictEqual([...DAP.decodeBase64(changed.data)], [...replacement]);
  ok(session, "writeMemory", {
    memoryReference: DAP.addressReference(address),
    data: DAP.encodeBase64(original),
  });

  const breakpoints = ok(session, "setInstructionBreakpoints", {
    breakpoints: [{ instructionReference: DAP.addressReference(originalPc) }],
  });
  assert.strictEqual(breakpoints.breakpoints[0].verified, true);
  ok(session, "setInstructionBreakpoints", { breakpoints: [] });

  console.log("DAP WebAssembly integration tests passed");
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
