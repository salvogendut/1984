"use strict";

/* Tests for the M4 relay protocol and the poll-driven browser bridge.
 * Run with: node test_m4_bridge.js */

const assert = require("node:assert/strict");
const P = require("./m4-relay-protocol.js");
const { M4Bridge, relayHealthEndpoint, validEndpoint, POLL } = require("./m4-bridge.js");

assert.equal(
  relayHealthEndpoint("wss://relay.example:1984/m4?token=secret#fragment"),
  "https://relay.example:1984/healthz"
);
assert.equal(
  relayHealthEndpoint("ws://127.0.0.1:1984/custom/path"),
  "http://127.0.0.1:1984/healthz"
);
assert.throws(() => relayHealthEndpoint("https://relay.example/m4"), /WS or WSS/);
assert.throws(() => validEndpoint("ftp://relay.example/m4"), /WS or WSS/);

function roundTrip(type, channel, payload) {
  const decoded = P.decode(P.encode(type, channel, 0, payload));
  assert.equal(decoded.type, type);
  assert.equal(decoded.channel, channel);
  assert.deepEqual(decoded.payload, new Uint8Array(payload));
}
roundTrip(P.Type.DNS, 0, P.encodeText("example.com"));
roundTrip(P.Type.TCP_OPEN, 3,
          P.concat(new Uint8Array([0]), P.u16(80), P.encodeText("1.2.3.4")));

class FakeWebSocket {
  static instances = [];

  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.bufferedAmount = 0;
    this.listeners = new Map();
    this.sent = [];
    FakeWebSocket.instances.push(this);
  }

  addEventListener(type, listener) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(listener);
  }

  emit(type, value = {}) {
    for (const listener of this.listeners.get(type) || []) listener(value);
  }

  open() {
    this.readyState = 1;
    this.emit("open");
  }

  send(value) { this.sent.push(new Uint8Array(value)); }

  recvFrame(frame) { this.emit("message", { data: frame.buffer }); }

  close() {
    if (this.readyState === 3) return;
    this.readyState = 3;
    this.emit("close");
  }
}

function makeBridge() {
  const bridge = new M4Bridge({
    WebSocketCtor: FakeWebSocket,
    endpoint: "ws://127.0.0.1:1984/m4",
  });
  return bridge;
}

function connectBridge(bridge) {
  bridge.setDevice(true);
  const socket = FakeWebSocket.instances.at(-1);
  socket.open();
  assert.equal(P.decode(socket.sent.shift()).type, P.Type.HELLO);
  socket.recvFrame(P.encode(P.Type.READY, 0, 0, P.u32(P.Feature.DNS | P.Feature.TCP)));
  assert.equal(bridge.isConnected(), true);
  assert.equal(bridge.status, "online");
  return socket;
}

{
  const bridge = makeBridge();
  const socket = connectBridge(bridge);

  // A dial before any DNS/TCP works is impossible for the guest (the M4 only
  // dials an IP), but the bridge must refuse when the relay is offline.
  bridge.setDevice(false);
  assert.equal(bridge.tcpConnect(1, new Uint8Array([93, 184, 216, 34]), 80), -1);
}

{
  // TCP open -> poll -> send -> rx -> recv
  const bridge = makeBridge();
  const socket = connectBridge(bridge);

  assert.equal(bridge.tcpConnect(1, new Uint8Array([93, 184, 216, 34]), 80), 1);
  const openFrame = P.decode(socket.sent.shift());
  assert.equal(openFrame.type, P.Type.TCP_OPEN);
  assert.equal(openFrame.channel, 1);
  assert.equal(openFrame.payload[0], 0);           // flags
  assert.equal(P.readU16(openFrame.payload, 1), 80);
  assert.equal(P.decodeText(openFrame.payload.subarray(3)), "93.184.216.34");

  // still connecting: no connect-completion flag yet
  assert.equal(bridge.poll(1) & (POLL.CONNECTED | POLL.FAILED), 0);

  socket.recvFrame(P.encode(P.Type.TCP_OPEN_RESULT, 1, 0,
                            P.concat(new Uint8Array([P.Status.OK]), new Uint8Array([10, 0, 0, 1]))));
  assert.equal(bridge.poll(1) & POLL.CONNECTED, POLL.CONNECTED);

  // sending on an open channel goes straight to the relay
  const payload = new Uint8Array([1, 2, 3, 4, 5]);
  assert.equal(bridge.send(1, payload), true);
  const sendFrame = P.decode(socket.sent.shift());
  assert.equal(sendFrame.type, P.Type.TCP_SEND);
  assert.deepEqual(sendFrame.payload, payload);

  // incoming bytes land in the RX queue and can be drained into a heap
  socket.recvFrame(P.encode(P.Type.TCP_DATA, 1, 0, new Uint8Array([0xde, 0xad, 0xbe, 0xef])));
  assert.equal(bridge.avail(1), 4);
  const heap = new Uint8Array(16);
  assert.equal(bridge.recv(1, heap, 0, 16), 4);
  assert.deepEqual(heap.subarray(0, 4), new Uint8Array([0xde, 0xad, 0xbe, 0xef]));

  // remote close surfaces as a poll flag and recv errors afterwards
  socket.recvFrame(P.encode(P.Type.TCP_CLOSED, 1, 0, new Uint8Array([P.Status.OK])));
  assert.equal(bridge.poll(1) & POLL.CLOSED, POLL.CLOSED);
  assert.equal(bridge.recv(1, heap, 0, 16), -1);

  bridge.close(1);
  const closeFrame = P.decode(socket.sent.shift());
  assert.equal(closeFrame.type, P.Type.TCP_CLOSE);
}

{
  // Async DNS through the relay
  const bridge = makeBridge();
  const socket = connectBridge(bridge);

  assert.equal(bridge.dns("example.com"), true);
  const dnsFrame = P.decode(socket.sent.shift());
  assert.equal(dnsFrame.type, P.Type.DNS);
  assert.equal(P.decodeText(dnsFrame.payload), "example.com");

  const out = new Uint8Array(4);
  assert.equal(bridge.dnsPoll(out), 0); // still pending
  socket.recvFrame(P.encode(P.Type.DNS_RESULT, 0, 0,
                            P.concat(new Uint8Array([P.Status.OK]), new Uint8Array([93, 184, 216, 34]))));
  assert.equal(bridge.dnsPoll(out), 1);
  assert.deepEqual(out, new Uint8Array([93, 184, 216, 34]));

  // failed lookup -> -1
  assert.equal(bridge.dns("does-not-exist.invalid"), true);
  socket.recvFrame(P.encode(P.Type.DNS_RESULT, 0, 0, new Uint8Array([P.Status.CONNECT_FAILED])));
  assert.equal(bridge.dnsPoll(out), -1);
}

// Endpoint validation + reconnection status
{
  const bridge = makeBridge();
  let seenStatus = [];
  bridge.onStatus((status, detail) => seenStatus.push(status));
  bridge.setEndpoint("ws://127.0.0.1:1984/m4");
  assert.equal(bridge.endpoint, "ws://127.0.0.1:1984/m4");
  assert.equal(bridge.setEndpoint("ftp://bad/m4"), false);
  assert.equal(seenStatus.at(-1), "error");
}

console.log("M4 bridge tests passed");
