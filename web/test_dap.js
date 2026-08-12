"use strict";

const assert = require("assert");
const DAP = require("./dap.js");
assert.strictEqual(DAP.PROTOCOL_VERSION, "1.71.0");

class MockBackend {
  constructor() {
    this.paused = false;
    this.reason = 0;
    this.registers = [
      0x1234, 0x2345, 0x3456, 0x4567, 0x5678, 0x6789, 0xff00,
      0x1000, 0xabcd, 0xbcde, 0xcdef, 0xdef0, 0x7f80, 1,
    ];
    this.memory = Uint8Array.from({ length: 0x10000 }, (_, index) => index & 0xff);
    this.breakpoints = new Map();
    this.nextSlot = 0;
    this.checkpoint = false;
    this.lastStep = "";
  }

  isPaused() { return this.paused; }
  pause() { this.paused = true; this.reason = 1; }
  continue() { this.paused = false; this.reason = 0; this.checkpoint = false; }
  stopReason() { return this.reason; }
  canStepBack() { return this.checkpoint; }
  register(index) { return this.registers[index]; }

  resumeStep(operation) {
    if (!this.paused) return -1;
    this.checkpoint = true;
    this.lastStep = operation;
    this.paused = false;
    this.reason = 0;
    return 0;
  }

  stepIn() { return this.resumeStep("stepIn"); }
  next() { return this.resumeStep("next"); }
  stepOut() { return this.resumeStep("stepOut"); }

  completeStep() {
    this.registers[7] = (this.registers[7] + 1) & 0xffff;
    this.paused = true;
    this.reason = 3;
  }

  stepBack() {
    if (!this.paused || !this.checkpoint) return -1;
    this.registers[7] = (this.registers[7] - 1) & 0xffff;
    this.checkpoint = false;
    this.reason = 4;
    return 0;
  }

  setBreakpoint(address) {
    for (const [slot, existing] of this.breakpoints)
      if (existing === address) return slot;
    if (this.breakpoints.size >= 16) return -1;
    const slot = this.nextSlot++;
    this.breakpoints.set(slot, address);
    return slot;
  }

  clearBreakpoint(slot) { this.breakpoints.delete(slot); }
  readMemory(address) { return this.memory[address]; }
  writeMemory(address, value) { this.memory[address] = value; return 0; }

  disassemble(address, count) {
    const lines = [];
    for (let index = 0; index < count; index++) {
      const current = (address + index) & 0xffff;
      const marker = current === this.registers[7] ? ">" : " ";
      lines.push(`${marker}${DAP.addressReference(current).slice(2)}  00           NOP`);
    }
    return lines.join("\n") + "\n";
  }
}

function ok(session, command, args) {
  const response = session.request(command, args);
  assert.strictEqual(response.type, "response");
  assert.strictEqual(response.command, command);
  assert.strictEqual(response.success, true,
    response.body && response.body.error ? response.body.error.format : response.message);
  return response.body;
}

function fails(session, command, args, message) {
  const response = session.request(command, args);
  assert.strictEqual(response.success, false);
  assert.match(response.body.error.format, message);
  return response;
}

const backend = new MockBackend();
const session = new DAP.Session(backend);

fails(session, "threads", {}, /initialize must be the first request/);
const capabilities = ok(session, "initialize", {
  adapterID: "1984-z80",
  linesStartAt1: true,
  columnsStartAt1: true,
  supportsMemoryReferences: true,
  supportsMemoryEvent: true,
});
assert.deepStrictEqual(capabilities, {
  supportsConfigurationDoneRequest: true,
  supportsStepBack: true,
  supportsReadMemoryRequest: true,
  supportsWriteMemoryRequest: true,
  supportsDisassembleRequest: true,
  supportsInstructionBreakpoints: true,
  supportsSteppingGranularity: true,
});
assert.strictEqual(capabilities.supportsDataBreakpoints, undefined);

ok(session, "attach", {});
let events = session.takeEvents();
assert.strictEqual(events.length, 1);
assert.strictEqual(events[0].event, "initialized");
ok(session, "configurationDone", {});
assert.deepStrictEqual(ok(session, "threads", {}), {
  threads: [{ id: 1, name: "Z80 CPU" }],
});
fails(session, "stackTrace", { threadId: 1 }, /must be stopped/);

ok(session, "pause", { threadId: 1 });
events = session.takeEvents();
assert.strictEqual(events.length, 1);
assert.strictEqual(events[0].event, "stopped");
assert.strictEqual(events[0].body.reason, "pause");
assert.strictEqual(events[0].body.threadId, 1);
assert.strictEqual(events[0].body.allThreadsStopped, true);

const stack = ok(session, "stackTrace", { threadId: 1, startFrame: 0, levels: 0 });
assert.strictEqual(stack.totalFrames, 1);
assert.strictEqual(stack.stackFrames.length, 1);
assert.strictEqual(stack.stackFrames[0].instructionPointerReference, "0x1000");
const oldFrameId = stack.stackFrames[0].id;
const scopes = ok(session, "scopes", { frameId: oldFrameId });
assert.strictEqual(scopes.scopes[0].presentationHint, "registers");
const variables = ok(session, "variables", {
  variablesReference: scopes.scopes[0].variablesReference,
});
assert.strictEqual(variables.variables.length, 14);
assert.deepStrictEqual(variables.variables.slice(0, 2).map(variable => variable.value),
                       ["0x1234", "0x2345"]);
assert.strictEqual(variables.variables[7].memoryReference, "0x1000");

const setBreakpoints = ok(session, "setInstructionBreakpoints", {
  breakpoints: [
    { instructionReference: "0x1010" },
    { instructionReference: "4096", offset: 0x20 },
  ],
});
assert.strictEqual(setBreakpoints.breakpoints.length, 2);
assert(setBreakpoints.breakpoints.every(breakpoint => breakpoint.verified));
assert.deepStrictEqual([...backend.breakpoints.values()], [0x1010, 0x1020]);
ok(session, "setInstructionBreakpoints", {
  breakpoints: [{ instructionReference: "0x2000" }],
});
assert.deepStrictEqual([...backend.breakpoints.values()], [0x2000]);

let memory = ok(session, "readMemory", {
  memoryReference: "0x00FE",
  count: 4,
});
assert.strictEqual(memory.address, "0x00FE");
assert.deepStrictEqual([...DAP.decodeBase64(memory.data)], [0xfe, 0xff, 0x00, 0x01]);
ok(session, "writeMemory", {
  memoryReference: "0x00FF",
  data: DAP.encodeBase64(Uint8Array.of(0xaa, 0xbb)),
});
assert.deepStrictEqual([...backend.memory.slice(0xff, 0x101)], [0xaa, 0xbb]);
events = session.takeEvents();
assert.strictEqual(events.length, 1);
assert.strictEqual(events[0].event, "memory");

memory = ok(session, "readMemory", { memoryReference: "0xFFFF", count: 3 });
assert.deepStrictEqual([...DAP.decodeBase64(memory.data)], [0xff]);
assert.strictEqual(memory.unreadableBytes, 2);
fails(session, "writeMemory", {
  memoryReference: "0xFFFF",
  data: DAP.encodeBase64(Uint8Array.of(1, 2)),
}, /crosses the end/);

const disassembly = ok(session, "disassemble", {
  memoryReference: "0x1000",
  instructionOffset: 2,
  instructionCount: 5,
});
assert.strictEqual(disassembly.instructions.length, 5);
assert.strictEqual(disassembly.instructions[0].address, "0x1002");
assert.strictEqual(disassembly.instructions[0].instruction, "NOP");
const preceding = ok(session, "disassemble", {
  memoryReference: "0x1000",
  instructionOffset: -3,
  instructionCount: 3,
});
assert.deepStrictEqual(preceding.instructions.map(instruction => instruction.address),
                       ["0x0FFD", "0x0FFE", "0x0FFF"]);

ok(session, "continue", { threadId: 1 });
fails(session, "scopes", { frameId: oldFrameId }, /must be stopped/);
ok(session, "pause", { threadId: 1 });
session.takeEvents();
fails(session, "scopes", { frameId: oldFrameId }, /expired/);

ok(session, "stepIn", { threadId: 1, granularity: "instruction" });
assert.strictEqual(backend.lastStep, "stepIn");
backend.completeStep();
session.sync();
events = session.takeEvents();
assert.strictEqual(events.length, 1);
assert.strictEqual(events[0].event, "stopped");
assert.strictEqual(events[0].body.reason, "step");

ok(session, "next", { threadId: 1, granularity: "instruction" });
assert.strictEqual(backend.lastStep, "next");
backend.completeStep();
session.sync();
session.takeEvents();
ok(session, "stepBack", { threadId: 1, granularity: "instruction" });
events = session.takeEvents();
assert.strictEqual(events[0].event, "stopped");
assert.match(events[0].body.description, /checkpoint/);

session.notifyWrite({
  label: "player_x", address: 0x4000, oldValue: 1, newValue: 2, pc: 0x1234,
});
events = session.takeEvents();
assert.deepStrictEqual(events.map(event => event.event), ["output", "memory"]);
assert.match(events[0].body.output, /player_x @0x4000/);

const framed = DAP.encodeMessage({
  seq: 1, type: "event", event: "output", body: { output: "Amstrad £" },
});
const expectedLength = new TextEncoder().encode(framed.split("\r\n\r\n")[1]).length;
assert.match(framed, new RegExp(`^Content-Length: ${expectedLength}\\r\\n\\r\\n`));
const parser = new DAP.MessageParser();
const encoded = new TextEncoder().encode(framed);
assert.deepStrictEqual(parser.push(encoded.slice(0, 17)), []);
assert.deepStrictEqual(parser.push(encoded.slice(17)), [{
  seq: 1, type: "event", event: "output", body: { output: "Amstrad £" },
}]);

const wireBackend = new MockBackend();
const connection = new DAP.Connection(new DAP.Session(wireBackend));
const wireParser = new DAP.MessageParser();
const wireOutput = connection.push(
  DAP.encodeMessage({
    seq: 1, type: "request", command: "initialize",
    arguments: { adapterID: "1984-z80" },
  }) +
  DAP.encodeMessage({ seq: 2, type: "request", command: "attach", arguments: {} })
);
const wireMessages = wireParser.push(wireOutput);
assert.deepStrictEqual(wireMessages.map(message => message.type),
                       ["response", "response", "event"]);
assert.deepStrictEqual(wireMessages.map(message => message.seq), [1, 2, 3]);
assert.strictEqual(wireMessages[2].event, "initialized");

console.log("DAP 1.71.0 session tests passed");
