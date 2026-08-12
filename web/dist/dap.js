(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.JS1984DAP = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  // In-process DAP 1.71.0 adapter plus the standard byte-stream framing used
  // by stdio and socket transports. The browser UI currently uses Session
  // directly; Connection is ready to sit behind a future external transport.
  const PROTOCOL_VERSION = "1.71.0";
  const THREAD_ID = 1;
  const MAX_ADDRESS = 0xffff;
  const MAX_MEMORY_TRANSFER = 0x10000;
  const MAX_DISASSEMBLY = 256;
  const REGISTER_NAMES = [
    "AF", "BC", "DE", "HL", "IX", "IY", "SP", "PC",
    "AF'", "BC'", "DE'", "HL'", "IR", "IM",
  ];

  function hex(value, width = 4) {
    return (value >>> 0).toString(16).toUpperCase().padStart(width, "0");
  }

  function addressReference(value) {
    return "0x" + hex(value & MAX_ADDRESS);
  }

  function parseReference(reference) {
    if (typeof reference !== "string" || !reference.trim())
      throw new Error("memoryReference must be a non-empty string");
    const text = reference.trim();
    if (!/^(?:0x[0-9a-f]+|[0-9]+)$/i.test(text))
      throw new Error("memoryReference must be decimal or 0x-prefixed hexadecimal");
    const value = Number.parseInt(text, text.toLowerCase().startsWith("0x") ? 16 : 10);
    if (!Number.isSafeInteger(value) || value < 0 || value > MAX_ADDRESS)
      throw new Error("memoryReference is outside the Z80 address space");
    return value;
  }

  const BASE64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  function encodeBase64(bytes) {
    let output = "";
    for (let offset = 0; offset < bytes.length; offset += 3) {
      const a = bytes[offset];
      const haveB = offset + 1 < bytes.length;
      const haveC = offset + 2 < bytes.length;
      const b = haveB ? bytes[offset + 1] : 0;
      const c = haveC ? bytes[offset + 2] : 0;
      output += BASE64[a >> 2];
      output += BASE64[((a & 3) << 4) | (b >> 4)];
      output += haveB ? BASE64[((b & 15) << 2) | (c >> 6)] : "=";
      output += haveC ? BASE64[c & 63] : "=";
    }
    return output;
  }

  function decodeBase64(text) {
    if (typeof text !== "string" || text.length % 4 !== 0 ||
        !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(text))
      throw new Error("data must be valid base64");
    const bytes = [];
    for (let offset = 0; offset < text.length; offset += 4) {
      const a = BASE64.indexOf(text[offset]);
      const b = BASE64.indexOf(text[offset + 1]);
      const c = text[offset + 2] === "=" ? 0 : BASE64.indexOf(text[offset + 2]);
      const d = text[offset + 3] === "=" ? 0 : BASE64.indexOf(text[offset + 3]);
      bytes.push((a << 2) | (b >> 4));
      if (text[offset + 2] !== "=") bytes.push(((b & 15) << 4) | (c >> 2));
      if (text[offset + 3] !== "=") bytes.push(((c & 3) << 6) | d);
    }
    return Uint8Array.from(bytes);
  }

  function parseDisassembly(text) {
    const instructions = [];
    for (const line of String(text || "").split("\n")) {
      const match = line.match(/^.([0-9A-Fa-f]{4})\s{2}/);
      if (!match) continue;
      const byteField = line.slice(7, 19);
      const bytes = byteField.match(/[0-9A-Fa-f]{2}/g) || [];
      instructions.push({
        address: Number.parseInt(match[1], 16),
        instructionBytes: bytes.join(" ").toUpperCase(),
        instruction: line.slice(20).trim() || "DB 00",
        byteLength: Math.max(1, bytes.length),
      });
    }
    return instructions;
  }

  function encodeMessage(message) {
    const json = JSON.stringify(message);
    const length = new TextEncoder().encode(json).length;
    return `Content-Length: ${length}\r\n\r\n${json}`;
  }

  class MessageParser {
    constructor() { this.buffer = new Uint8Array(0); }

    push(chunk) {
      const incoming = typeof chunk === "string" ? new TextEncoder().encode(chunk) : chunk;
      if (!(incoming instanceof Uint8Array))
        throw new TypeError("DAP input must be a string or Uint8Array");
      const merged = new Uint8Array(this.buffer.length + incoming.length);
      merged.set(this.buffer);
      merged.set(incoming, this.buffer.length);
      this.buffer = merged;
      const messages = [];
      const separator = new Uint8Array([13, 10, 13, 10]);
      while (this.buffer.length) {
        let headerEnd = -1;
        outer: for (let i = 0; i <= this.buffer.length - separator.length; i++) {
          for (let j = 0; j < separator.length; j++)
            if (this.buffer[i + j] !== separator[j]) continue outer;
          headerEnd = i;
          break;
        }
        if (headerEnd < 0) break;
        const header = new TextDecoder("ascii").decode(this.buffer.slice(0, headerEnd));
        const match = header.match(/(?:^|\r\n)Content-Length: ([0-9]+)(?:\r\n|$)/i);
        if (!match) throw new Error("DAP message is missing Content-Length");
        const length = Number(match[1]);
        const contentStart = headerEnd + separator.length;
        if (this.buffer.length < contentStart + length) break;
        const content = new TextDecoder().decode(
          this.buffer.slice(contentStart, contentStart + length)
        );
        messages.push(JSON.parse(content));
        this.buffer = this.buffer.slice(contentStart + length);
      }
      return messages;
    }
  }

  class Connection {
    constructor(session) {
      this.session = session;
      this.parser = new MessageParser();
    }

    push(chunk) {
      const output = [];
      for (const request of this.parser.push(chunk)) {
        output.push(encodeMessage(this.session.dispatch(request)));
        output.push(...this.session.takeEvents().map(encodeMessage));
      }
      return output.join("");
    }

    sync() {
      this.session.sync();
      return this.session.takeEvents().map(encodeMessage).join("");
    }
  }

  class Session {
    constructor(backend) {
      this.backend = backend;
      this.sequence = 1;
      this.clientSequence = 1;
      this.initialized = false;
      this.attached = false;
      this.configured = false;
      this.terminated = false;
      this.clientCapabilities = {};
      this.pendingEvents = [];
      this.running = !backend.isPaused();
      this.stopEpoch = 1;
      this.breakpointSlots = new Map();
      this.breakpointIds = new Map();
      this.nextBreakpointId = 1;
    }

    request(command, args = {}) {
      return this.dispatch({
        seq: this.clientSequence++,
        type: "request",
        command,
        arguments: args,
      });
    }

    dispatch(request) {
      if (!request || request.type !== "request" ||
          !Number.isInteger(request.seq) || request.seq < 1 ||
          typeof request.command !== "string")
        throw new Error("invalid DAP request");
      try {
        const body = this._handle(request.command, request.arguments || {});
        return this._response(request, true, body);
      } catch (error) {
        return this._response(request, false, undefined,
          error.dapMessage || "requestFailed", error.message);
      }
    }

    takeEvents() {
      return this.pendingEvents.splice(0).map(event => ({
        seq: this.sequence++,
        type: "event",
        event: event.event,
        ...(event.body === undefined ? {} : { body: event.body }),
      }));
    }

    sync() {
      if (!this.attached || this.terminated) return;
      const paused = this.backend.isPaused();
      if (this.running && paused) {
        this.running = false;
        this._stoppedFromBackend();
      } else if (!this.running && !paused) {
        this.running = true;
        this._invalidateStoppedState();
        this._queue("continued", { threadId: THREAD_ID, allThreadsContinued: true });
      }
    }

    notifyWrite(write) {
      if (!this.initialized || this.terminated) return;
      const location = addressReference(write.address);
      this._queue("output", {
        category: "console",
        output: `${write.label} @${location}: ${hex(write.oldValue, 2)} -> ${hex(write.newValue, 2)} (PC ${addressReference(write.pc)})\n`,
      });
      if (this.clientCapabilities.supportsMemoryEvent) {
        this._queue("memory", {
          memoryReference: location,
          offset: 0,
          count: 1,
        });
      }
    }

    _response(request, success, body, message, detail) {
      const response = {
        seq: this.sequence++,
        type: "response",
        request_seq: request.seq,
        success,
        command: request.command,
      };
      if (body !== undefined) response.body = body;
      if (!success) {
        response.message = message || "requestFailed";
        response.body = { error: { id: 1984, format: detail || "Request failed", showUser: true } };
      }
      return response;
    }

    _queue(event, body) { this.pendingEvents.push({ event, body }); }

    _fail(message, dapMessage = "requestFailed") {
      const error = new Error(message);
      error.dapMessage = dapMessage;
      throw error;
    }

    _requireAttached() {
      if (!this.attached || this.terminated) this._fail("No CPC debuggee is attached");
    }

    _requireStopped() {
      this._requireAttached();
      if (this.running || !this.backend.isPaused())
        this._fail("The CPC must be stopped for this request", "notStopped");
    }

    _thread(threadId) {
      if (threadId !== THREAD_ID) this._fail(`Unknown thread ${threadId}`);
    }

    _invalidateStoppedState() { this.stopEpoch++; }
    _frameId() { return this.stopEpoch * 16 + 1; }
    _registersReference() { return this.stopEpoch * 16 + 2; }

    _stopped(reason, description, hitBreakpointIds) {
      this._invalidateStoppedState();
      const body = {
        reason,
        description,
        threadId: THREAD_ID,
        allThreadsStopped: true,
      };
      if (hitBreakpointIds && hitBreakpointIds.length)
        body.hitBreakpointIds = hitBreakpointIds;
      this._queue("stopped", body);
    }

    _stoppedFromBackend() {
      const reason = this.backend.stopReason();
      if (reason === 2) {
        const pc = this.backend.register(7) & MAX_ADDRESS;
        const id = this.breakpointIds.get(pc);
        this._stopped("instruction breakpoint", "Paused on instruction breakpoint",
          id === undefined ? undefined : [id]);
      } else if (reason === 1) {
        this._stopped("pause", "Execution paused");
      } else {
        this._stopped("step", "Instruction step completed");
      }
    }

    _handle(command, args) {
      if (command === "initialize") {
        if (this.initialized) this._fail("initialize may only be sent once");
        this.initialized = true;
        this.clientCapabilities = { ...args };
        return {
          supportsConfigurationDoneRequest: true,
          supportsStepBack: true,
          supportsReadMemoryRequest: true,
          supportsWriteMemoryRequest: true,
          supportsDisassembleRequest: true,
          supportsInstructionBreakpoints: true,
          supportsSteppingGranularity: true,
        };
      }
      if (!this.initialized)
        this._fail("initialize must be the first request");

      switch (command) {
      case "attach":
        if (this.attached) this._fail("A CPC debuggee is already attached");
        this.attached = true;
        this.running = !this.backend.isPaused();
        this._queue("initialized");
        return {};
      case "configurationDone":
        this._requireAttached();
        this.configured = true;
        return {};
      case "disconnect":
        if (this.attached && this.backend.isPaused() && !args.suspendDebuggee)
          this.backend.continue();
        this.attached = false;
        this.terminated = true;
        return {};
      case "threads":
        this._requireAttached();
        return { threads: [{ id: THREAD_ID, name: "Z80 CPU" }] };
      case "stackTrace":
        return this._stackTrace(args);
      case "scopes":
        return this._scopes(args);
      case "variables":
        return this._variables(args);
      case "pause":
        this._thread(args.threadId);
        this._requireAttached();
        if (!this.backend.isPaused()) {
          this.backend.pause();
          this.running = false;
          this._stopped("pause", "Execution paused by user request");
        }
        return undefined;
      case "continue":
        this._thread(args.threadId);
        this._requireStopped();
        this.backend.continue();
        this.running = true;
        this._invalidateStoppedState();
        return { allThreadsContinued: true };
      case "stepIn":
        return this._resumeStep(args, "stepIn");
      case "next":
        return this._resumeStep(args, "next");
      case "stepOut":
        return this._resumeStep(args, "stepOut");
      case "stepBack":
      case "reverseContinue":
        this._thread(args.threadId);
        this._requireStopped();
        if (!this.backend.canStepBack() || this.backend.stepBack() !== 0)
          this._fail("No reverse-execution checkpoint is available");
        this.running = false;
        this._stopped("step", "Restored the previous instruction checkpoint");
        return undefined;
      case "setInstructionBreakpoints":
        return this._setInstructionBreakpoints(args);
      case "readMemory":
        return this._readMemory(args);
      case "writeMemory":
        return this._writeMemory(args);
      case "disassemble":
        return this._disassemble(args);
      default:
        this._fail(`DAP request '${command}' is not supported`);
      }
    }

    _resumeStep(args, operation) {
      this._thread(args.threadId);
      this._requireStopped();
      if (args.granularity && !["statement", "line", "instruction"].includes(args.granularity))
        this._fail(`Unsupported stepping granularity '${args.granularity}'`);
      const result = this.backend[operation]();
      if (result < 0) {
        if (result === -2) this._fail("No temporary breakpoint channel is available");
        this._fail(`${operation} was rejected by the Z80 debugger`);
      }
      this.running = true;
      this._invalidateStoppedState();
      return undefined;
    }

    _stackTrace(args) {
      this._thread(args.threadId);
      this._requireStopped();
      const start = args.startFrame || 0;
      const levels = args.levels || 1;
      if (start > 0 || levels === 0) return { stackFrames: [], totalFrames: 1 };
      const pc = this.backend.register(7) & MAX_ADDRESS;
      return {
        stackFrames: [{
          id: this._frameId(),
          name: `Z80 @ ${addressReference(pc)}`,
          line: 0,
          column: 0,
          instructionPointerReference: addressReference(pc),
        }],
        totalFrames: 1,
      };
    }

    _scopes(args) {
      this._requireStopped();
      if (args.frameId !== this._frameId()) this._fail("Stack frame reference has expired");
      return { scopes: [{
        name: "Registers",
        presentationHint: "registers",
        variablesReference: this._registersReference(),
        namedVariables: REGISTER_NAMES.length,
        expensive: false,
      }] };
    }

    _variables(args) {
      this._requireStopped();
      if (args.variablesReference !== this._registersReference())
        this._fail("Variable reference has expired");
      return { variables: REGISTER_NAMES.map((name, index) => {
        const value = this.backend.register(index);
        const width = index === 13 ? 1 : 4;
        const variable = {
          name,
          value: "0x" + hex(value, width),
          variablesReference: 0,
        };
        if (index === 6 || index === 7 || index === 4 || index === 5)
          variable.memoryReference = addressReference(value);
        return variable;
      }) };
    }

    _setInstructionBreakpoints(args) {
      this._requireAttached();
      if (!Array.isArray(args.breakpoints)) this._fail("breakpoints must be an array");
      for (const slot of this.breakpointSlots.values()) this.backend.clearBreakpoint(slot);
      this.breakpointSlots.clear();
      const results = [];
      for (const requested of args.breakpoints) {
        try {
          let address = parseReference(requested.instructionReference);
          address += requested.offset || 0;
          if (!Number.isInteger(address) || address < 0 || address > MAX_ADDRESS)
            throw new Error("instruction address is outside the Z80 address space");
          const slot = this.backend.setBreakpoint(address);
          if (slot < 0) throw new Error("all 16 breakpoint channels are in use");
          this.breakpointSlots.set(address, slot);
          if (!this.breakpointIds.has(address))
            this.breakpointIds.set(address, this.nextBreakpointId++);
          results.push({
            id: this.breakpointIds.get(address),
            verified: true,
            instructionReference: addressReference(address),
          });
        } catch (error) {
          results.push({
            verified: false,
            message: error.message,
            reason: "failed",
            instructionReference: requested.instructionReference,
          });
        }
      }
      return { breakpoints: results };
    }

    _memoryRange(args, dataLength) {
      const base = parseReference(args.memoryReference);
      const offset = args.offset || 0;
      const start = base + offset;
      if (!Number.isSafeInteger(start) || start < 0 || start > MAX_ADDRESS)
        this._fail("Memory offset is outside the Z80 address space");
      const count = dataLength === undefined ? args.count : dataLength;
      if (!Number.isSafeInteger(count) || count < 0 || count > MAX_MEMORY_TRANSFER)
        this._fail("Memory transfer count must be between 0 and 65536 bytes");
      return { start, count, available: MAX_ADDRESS + 1 - start };
    }

    _readMemory(args) {
      this._requireStopped();
      const range = this._memoryRange(args);
      const readable = Math.min(range.count, range.available);
      const bytes = new Uint8Array(readable);
      for (let i = 0; i < readable; i++) bytes[i] = this.backend.readMemory(range.start + i);
      const body = { address: addressReference(range.start), data: encodeBase64(bytes) };
      if (readable < range.count) body.unreadableBytes = range.count - readable;
      return body;
    }

    _writeMemory(args) {
      this._requireStopped();
      const bytes = decodeBase64(args.data);
      const range = this._memoryRange(args, bytes.length);
      if (bytes.length > range.available && !args.allowPartial)
        this._fail("The requested write crosses the end of Z80 memory");
      const writable = Math.min(bytes.length, range.available);
      for (let i = 0; i < writable; i++) {
        if (this.backend.writeMemory(range.start + i, bytes[i]) !== 0)
          this._fail(`Memory write failed at ${addressReference(range.start + i)}`);
      }
      if (this.clientCapabilities.supportsMemoryEvent && writable) {
        this._queue("memory", {
          memoryReference: addressReference(range.start),
          offset: 0,
          count: writable,
        });
      }
      return args.allowPartial ? { offset: 0, bytesWritten: writable } : {};
    }

    _disassemble(args) {
      this._requireStopped();
      const base = parseReference(args.memoryReference);
      const byteOffset = args.offset || 0;
      let address = base + byteOffset;
      if (!Number.isInteger(address) || address < 0 || address > MAX_ADDRESS)
        this._fail("Disassembly offset is outside the Z80 address space");
      const skip = args.instructionOffset || 0;
      if (!Number.isInteger(skip) || Math.abs(skip) > MAX_DISASSEMBLY)
        this._fail("instructionOffset must be between -256 and 256");
      const count = args.instructionCount;
      if (!Number.isInteger(count) || count < 0 || count > MAX_DISASSEMBLY)
        this._fail("instructionCount must be between 0 and 256");

      const collect = wanted => {
        const output = [];
        while (output.length < wanted) {
          const batch = parseDisassembly(
            this.backend.disassemble(address, Math.min(12, wanted - output.length))
          );
          if (!batch.length) {
            output.push({
              address,
              instructionBytes: "",
              instruction: "unavailable",
              byteLength: 1,
              presentationHint: "invalid",
            });
          } else {
            output.push(...batch.slice(0, wanted - output.length));
          }
          const last = output[output.length - 1];
          address = (last.address + last.byteLength) & MAX_ADDRESS;
        }
        return output;
      };
      if (skip < 0) {
        for (let preceding = 0; preceding < -skip; preceding++) {
          let previous = (address - 1) & MAX_ADDRESS;
          // Z80 instructions are at most four bytes. Prefer the longest valid
          // decode that ends at the current address; bytes before an arbitrary
          // memory reference cannot otherwise be unambiguously aligned.
          for (let length = 4; length >= 1; length--) {
            const candidate = (address - length) & MAX_ADDRESS;
            const decoded = parseDisassembly(this.backend.disassemble(candidate, 1))[0];
            if (decoded && ((decoded.address + decoded.byteLength) & MAX_ADDRESS) === address) {
              previous = candidate;
              break;
            }
          }
          address = previous;
        }
      } else if (skip) {
        collect(skip);
      }
      const instructions = collect(count).map(instruction => ({
        address: addressReference(instruction.address),
        instructionBytes: instruction.instructionBytes,
        instruction: instruction.instruction,
        ...(instruction.presentationHint ? { presentationHint: instruction.presentationHint } : {}),
      }));
      return { instructions };
    }
  }

  return {
    PROTOCOL_VERSION,
    THREAD_ID,
    Session,
    Connection,
    MessageParser,
    encodeMessage,
    encodeBase64,
    decodeBase64,
    parseReference,
    addressReference,
    parseDisassembly,
  };
});
