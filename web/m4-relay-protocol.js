(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  root.JSWsUnapiRelayProtocol = api;
  root.JS1984M4Protocol = api;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  /* Shared relay protocol (magic 0x83) spoken by the M4 internet bridge.
   *
   * This is the same versioned binary protocol used by the standalone
   * ws-unapi-relay service, so a single deployed relay instance can
   * serve both the 1983 MSX UNAPI bridge and the 1984 M4 bridge. Frame
   * layout (8-byte header + payload):
   *   byte 0  magic     0x83
   *   byte 1  version   1
   *   byte 2  type
   *   byte 3  channel   (0 = DNS, 1..4 = TCP sockets)
   *   byte 4..5  request id, little-endian (0 is fine for the M4 bridge)
   *   byte 6..7  payload length, little-endian
   *   bytes 8..  payload
   */

  const MAGIC = 0x83;
  const VERSION = 1;
  const HEADER_SIZE = 8;

  const Type = Object.freeze({
    HELLO: 0x01,
    DNS: 0x10,
    TCP_OPEN: 0x20,
    TCP_SEND: 0x21,
    TCP_CLOSE: 0x22,
    UDP_OPEN: 0x30,
    UDP_SEND: 0x31,
    UDP_CLOSE: 0x32,

    READY: 0x81,
    DNS_RESULT: 0x90,
    TCP_OPEN_RESULT: 0xa0,
    TCP_DATA: 0xa1,
    TCP_CLOSED: 0xa2,
    UDP_OPEN_RESULT: 0xb0,
    UDP_DATA: 0xb1,
    UDP_CLOSED: 0xb2,
  });

  // Compact relay status values shared by the browser bridge and relay.
  const Status = Object.freeze({
    OK: 0x00,
    BAD_FRAME: 0x01,
    BAD_OPCODE: 0x02,
    BAD_LENGTH: 0x03,
    BAD_CHANNEL: 0x04,
    NO_SLOT: 0x05,
    WIFI_DOWN: 0x06,
    CONNECT_FAILED: 0x07,
    IO_ERROR: 0x08,
    UNSUPPORTED: 0x09,
    BUSY: 0x0a,
    BAD_ARGUMENT: 0x0b,
  });

  const Feature = Object.freeze({ DNS: 1, TCP: 2, UDP: 4 });

  function bytes(value) {
    if (value instanceof Uint8Array) return value;
    if (value instanceof ArrayBuffer) return new Uint8Array(value);
    if (ArrayBuffer.isView(value))
      return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    throw new TypeError("relay frame must be binary");
  }

  function encode(type, channel = 0, request = 0, payload = new Uint8Array()) {
    const body = bytes(payload);
    if (body.length > 0xffff) throw new RangeError("relay payload is too large");
    const frame = new Uint8Array(HEADER_SIZE + body.length);
    frame[0] = MAGIC;
    frame[1] = VERSION;
    frame[2] = type & 0xff;
    frame[3] = channel & 0xff;
    frame[4] = request & 0xff;
    frame[5] = (request >>> 8) & 0xff;
    frame[6] = body.length & 0xff;
    frame[7] = (body.length >>> 8) & 0xff;
    frame.set(body, HEADER_SIZE);
    return frame;
  }

  function decode(value) {
    const frame = bytes(value);
    if (frame.length < HEADER_SIZE || frame[0] !== MAGIC || frame[1] !== VERSION)
      throw new Error("invalid relay frame header");
    const length = frame[6] | (frame[7] << 8);
    if (frame.length !== HEADER_SIZE + length)
      throw new Error("invalid relay frame length");
    return {
      type: frame[2],
      channel: frame[3],
      request: frame[4] | (frame[5] << 8),
      payload: frame.subarray(HEADER_SIZE),
    };
  }

  function u16(value) {
    return new Uint8Array([value & 0xff, (value >>> 8) & 0xff]);
  }

  function u32(value) {
    return new Uint8Array([
      value & 0xff,
      (value >>> 8) & 0xff,
      (value >>> 16) & 0xff,
      (value >>> 24) & 0xff,
    ]);
  }

  function readU16(value, offset = 0) {
    const data = bytes(value);
    return data[offset] | (data[offset + 1] << 8);
  }

  function readU32(value, offset = 0) {
    const data = bytes(value);
    return (data[offset] |
            (data[offset + 1] << 8) |
            (data[offset + 2] << 16) |
            (data[offset + 3] << 24)) >>> 0;
  }

  function concat(...parts) {
    const arrays = parts.map(bytes);
    const result = new Uint8Array(arrays.reduce((sum, item) => sum + item.length, 0));
    let offset = 0;
    for (const item of arrays) {
      result.set(item, offset);
      offset += item.length;
    }
    return result;
  }

  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  return {
    MAGIC, VERSION, HEADER_SIZE, Type, Status, Feature,
    bytes, encode, decode, u16, u32, readU16, readU32, concat,
    encodeText: value => encoder.encode(value),
    decodeText: value => decoder.decode(bytes(value)),
  };
}));
