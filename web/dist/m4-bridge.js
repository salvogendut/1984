"use strict";

/* Browser bridge for the 1984 M4 board's network stack.
 *
 * Owns the WebSocket connection to the restricted relay and exposes the
 * poll-driven API the C side expects (see m4_web.h). Installed on
 * globalThis.JS1984M4Bridge; the C EM_JS boundary (m4_web.c) calls it
 * synchronously while the relay I/O happens between emulator frames.
 */
(function (root, factory) {
  const P = root.JS1984M4Protocol ||
    (typeof require === "function" ? require("./m4-relay-protocol.js") : null);
  const api = factory(P);
  if (typeof module === "object" && module.exports) module.exports = api;
  root.JS1984M4 = api;
  if (!root.JS1984M4Bridge) root.JS1984M4Bridge = api.createBridge();
}(typeof globalThis !== "undefined" ? globalThis : this, function (P) {
  if (!P) throw new Error("M4 relay protocol is unavailable");

  const POLL = { CONNECTED: 0x01, FAILED: 0x02, CLOSED: 0x04, RX: 0x08 };

  class ByteQueue {
    constructor(limit) {
      this.limit = limit;
      this.parts = [];
      this.offset = 0;
      this.length = 0;
    }

    push(value) {
      const data = P.bytes(value);
      if (this.length + data.length > this.limit) return false;
      if (data.length) {
        this.parts.push(data);
        this.length += data.length;
      }
      return true;
    }

    read(target, offset, maximum) {
      let written = 0;
      while (written < maximum && this.parts.length) {
        const part = this.parts[0];
        const count = Math.min(maximum - written, part.length - this.offset);
        target.set(part.subarray(this.offset, this.offset + count), offset + written);
        written += count;
        this.offset += count;
        this.length -= count;
        if (this.offset === part.length) {
          this.parts.shift();
          this.offset = 0;
        }
      }
      return written;
    }
  }

  function pageDefaultEndpoint(locationObject) {
    if (!locationObject || !locationObject.host) return "";
    const scheme = locationObject.protocol === "https:" ? "wss:" : "ws:";
    return scheme + "//" + locationObject.host + "/unapi";
  }

  function validEndpoint(value) {
    if (!value) return "";
    const url = new URL(value, typeof location === "object" ? location.href : undefined);
    if (url.protocol !== "ws:" && url.protocol !== "wss:")
      throw new Error("M4 relay must use WS or WSS");
    if (typeof location === "object" && location.protocol === "https:" &&
        url.protocol !== "wss:")
      throw new Error("HTTPS pages require a secure WSS relay");
    return url.href;
  }

  function relayHealthEndpoint(value) {
    const endpoint = validEndpoint(value);
    if (!endpoint) throw new Error("No M4 relay endpoint");
    const url = new URL(endpoint);
    url.protocol = url.protocol === "wss:" ? "https:" : "http:";
    url.username = "";
    url.password = "";
    url.pathname = "/healthz";
    url.search = "";
    url.hash = "";
    return url.href;
  }

  class M4Bridge {
    constructor(options = {}) {
      this.WebSocketCtor = options.WebSocketCtor ||
        (typeof globalThis !== "undefined" &&
         typeof globalThis.WebSocket === "function"
          ? globalThis.WebSocket : null);
      this.endpoint = options.endpoint || pageDefaultEndpoint(
        typeof location === "object" ? location : null
      );
      this.maxQueueBytes = options.maxQueueBytes || 65536;
      this.maxBufferedAmount = options.maxBufferedAmount || 262144;
      this.handshakeTimeoutMs = options.handshakeTimeoutMs || 10000;
      this.socket = null;
      this.enabled = false;
      this.ready = false;
      this.activity = false;
      this.status = "disabled";
      this.statusDetail = "";
      this.statusListeners = new Set();
      this.tcp = new Map();
      this.dnsPending = null;
      this.reconnectTimer = 0;
      this.handshakeTimer = 0;
      this.reconnectDelay = 500;
    }

    onStatus(listener) {
      this.statusListeners.add(listener);
      listener(this.status, this.statusDetail);
      return () => this.statusListeners.delete(listener);
    }

    _setStatus(status, detail = "") {
      if (this.status === status && this.statusDetail === detail) return;
      this.status = status;
      this.statusDetail = detail;
      for (const listener of this.statusListeners) listener(status, detail);
    }

    setEndpoint(value) {
      let endpoint;
      try {
        endpoint = validEndpoint(value);
      } catch (error) {
        this.endpoint = "";
        this._disconnect(false);
        this._setStatus("error", error.message);
        return false;
      }
      if (endpoint === this.endpoint) return true;
      this.endpoint = endpoint;
      this._disconnect(false);
      if (this.enabled) this.connect();
      return true;
    }

    setDevice(enabled) {
      this.enabled = Boolean(enabled);
      if (!this.enabled) {
        this._disconnect(false);
        this._setStatus("disabled");
      } else {
        this.connect();
      }
    }

    isConnected() { return this.ready; }

    takeActivity() {
      const active = this.activity;
      this.activity = false;
      return active;
    }

    connect() {
      if (!this.enabled || this.ready ||
          (this.socket && this.socket.readyState === 0)) return;
      clearTimeout(this.reconnectTimer);
      if (!this.endpoint || !this.WebSocketCtor) {
        this._setStatus("offline", this.endpoint ? "WebSocket unavailable" : "No relay endpoint");
        return;
      }

      let socket;
      try {
        socket = new this.WebSocketCtor(this.endpoint);
      } catch (error) {
        this._setStatus("error", error.message);
        this._scheduleReconnect();
        return;
      }
      this.socket = socket;
      socket.binaryType = "arraybuffer";
      this._setStatus("connecting");
      clearTimeout(this.handshakeTimer);
      this.handshakeTimer = setTimeout(() => {
        if (this.socket === socket && !this.ready)
          socket.close(1002, "relay handshake timeout");
      }, this.handshakeTimeoutMs);

      socket.addEventListener("open", () => {
        if (this.socket !== socket) return;
        this._send(P.encode(P.Type.HELLO));
      });
      socket.addEventListener("message", event => {
        if (this.socket !== socket) return;
        try {
          this._handleFrame(P.decode(event.data));
        } catch (error) {
          this._setStatus("error", error.message);
          socket.close(1002, "invalid relay frame");
        }
      });
      socket.addEventListener("error", () => {
        if (this.socket !== socket) return;
        this._setStatus("error",
          "Relay connection failed (check address, port and certificate)");
      });
      socket.addEventListener("close", event => {
        if (this.socket !== socket) return;
        clearTimeout(this.handshakeTimer);
        this.socket = null;
        const wasReady = this.ready;
        this.ready = false;
        this.dnsPending = null;
        this._markChannels("error");
        if (!this.enabled) {
          this._setStatus("disabled");
          return;
        }
        const code = event && event.code ? " (code " + event.code + ")" : "";
        const reason = event && event.reason ? ": " + event.reason : "";
        const detail = wasReady
          ? "Relay disconnected" + code + reason
          : "Relay handshake failed" + code + reason +
            " - check the origin is in the relay's UNAPI_ORIGINS";
        this._setStatus("offline", detail);
        this._scheduleReconnect();
      });
    }

    resetChannels() {
      for (const channel of this.tcp.keys())
        this._send(P.encode(P.Type.TCP_CLOSE, channel));
      this.tcp.clear();
      this.dnsPending = null;
    }

    /* ---- C API (m4_web.c) ---- */

    tcpConnect(channel, ip, port) {
      const existing = this.tcp.get(channel);
      if (existing && existing.state === "open") return 0;
      if (existing) return -1;   /* already connecting / failed — no re-dial */
      const state = {
        state: "connecting",
        rx: new ByteQueue(this.maxQueueBytes),
        address: ip,
        port,
      };
      this.tcp.set(channel, state);
      // Shared relay TCP_OPEN payload: flags, port, hostname. The M4 dials by
      // IP literal, which the relay accepts as a numeric hostname.
      const hostname = String(ip[0]) + "." + String(ip[1]) + "." +
                       String(ip[2]) + "." + String(ip[3]);
      if (!this._send(P.encode(P.Type.TCP_OPEN, channel, 0,
                               P.concat(new Uint8Array([0]), P.u16(port),
                                        P.encodeText(hostname))))) {
        this.tcp.delete(channel);
        return -1;
      }
      this.activity = true;
      return 1;
    }

    poll(channel) {
      const state = this.tcp.get(channel);
      if (!state) return 0;
      let flags = 0;
      if (state.state === "open") flags |= POLL.CONNECTED;
      else if (state.state === "error") flags |= POLL.FAILED;
      else if (state.state === "closed") flags |= POLL.CLOSED;
      if (state.rx.length > 0) flags |= POLL.RX;
      return flags;
    }

    avail(channel) {
      const state = this.tcp.get(channel);
      return state ? state.rx.length : 0;
    }

    send(channel, data) {
      const state = this.tcp.get(channel);
      if (!state || state.state !== "open") return false;
      if (!this._send(P.encode(P.Type.TCP_SEND, channel, 0, data)))
        return false;
      this.activity = true;
      return true;
    }

    recv(channel, heap, pointer, maximum) {
      const state = this.tcp.get(channel);
      if (!state) return -1;
      if (state.rx.length)
        return state.rx.read(heap, pointer, maximum);
      if (state.state === "error" || state.state === "closed") return -1;
      return 0;
    }

    close(channel) {
      if (this.tcp.has(channel))
        this._send(P.encode(P.Type.TCP_CLOSE, channel));
      this.tcp.delete(channel);
    }

    dns(host) {
      if (this.dnsPending) return false;
      this.dnsPending = { host, result: null };
      if (!this._send(P.encode(P.Type.DNS, 0, 0, P.encodeText(host)))) {
        this.dnsPending = null;
        return false;
      }
      this.activity = true;
      return true;
    }

    dnsPoll(out4) {
      const pending = this.dnsPending;
      if (!pending) return 0;
      if (!pending.result) return 0;
      this.dnsPending = null;
      if (pending.result.status === P.Status.OK && pending.result.ip) {
        out4.set(pending.result.ip, 0);
        return 1;
      }
      return -1;
    }

    /* ---- relay frames ---- */

    _handleFrame(frame) {
      switch (frame.type) {
        case P.Type.READY:
          clearTimeout(this.handshakeTimer);
          this.ready = true;
          this.reconnectDelay = 500;
          this._setStatus("online");
          this.activity = true;
          return;
        case P.Type.DNS_RESULT:
          this._handleDnsResult(frame);
          return;
        case P.Type.TCP_OPEN_RESULT:
          this._handleTcpOpenResult(frame);
          return;
        case P.Type.TCP_DATA:
          this._handleTcpData(frame);
          return;
        case P.Type.TCP_CLOSED:
          this._handleTcpClosed(frame);
          return;
        default:
          throw new Error("unknown relay message " + frame.type);
      }
    }

    _handleDnsResult(frame) {
      const pending = this.dnsPending;
      if (!pending || frame.payload.length < 1) return;
      const status = frame.payload[0];
      const ip = status === P.Status.OK && frame.payload.length >= 5
        ? frame.payload.subarray(1, 5) : null;
      pending.result = { status, ip };
      this.activity = true;
    }

    _handleTcpOpenResult(frame) {
      const state = this.tcp.get(frame.channel);
      if (!state || frame.payload.length < 1) return;
      const status = frame.payload[0];
      state.state = status === P.Status.OK ? "open" : "error";
      this.activity = true;
    }

    _handleTcpData(frame) {
      const state = this.tcp.get(frame.channel);
      if (!state || !state.rx.push(frame.payload)) {
        if (state) state.state = "error";
        this._send(P.encode(P.Type.TCP_CLOSE, frame.channel));
      }
      this.activity = true;
    }

    _handleTcpClosed(frame) {
      const state = this.tcp.get(frame.channel);
      if (!state) return;
      const status = frame.payload[0] || P.Status.OK;
      state.state = status === P.Status.OK ? "closed" : "error";
      this.activity = true;
    }

    /* ---- plumbing ---- */

    _send(frame) {
      if (!this.socket || this.socket.readyState !== 1) return false;
      if (this.socket.bufferedAmount > this.maxBufferedAmount) return false;
      this.socket.send(frame);
      return true;
    }

    _scheduleReconnect() {
      clearTimeout(this.reconnectTimer);
      if (!this.enabled) return;
      const delay = this.reconnectDelay;
      this.reconnectDelay = Math.min(this.reconnectDelay * 2, 10000);
      this.reconnectTimer = setTimeout(() => this.connect(), delay);
    }

    _disconnect(sendClose) {
      clearTimeout(this.reconnectTimer);
      clearTimeout(this.handshakeTimer);
      const socket = this.socket;
      this.socket = null;
      this.ready = false;
      this.dnsPending = null;
      this._markChannels("error");
      if (socket && sendClose && socket.readyState === 1)
        socket.close(1000, "network disconnected");
      else if (socket)
        socket.close();
    }

    _markChannels(state) {
      for (const channel of this.tcp.values()) channel.state = state;
    }
  }

  return {
    ByteQueue,
    M4Bridge,
    createBridge: options => new M4Bridge(options),
    pageDefaultEndpoint,
    relayHealthEndpoint,
    validEndpoint,
    POLL,
  };
}));
