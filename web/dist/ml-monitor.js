(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  root.JS1984Monitor = api;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  function parseAddress(value) {
    const text = String(value || "").trim().replace(/^0x/i, "").replace(/^&/, "");
    if (!/^[0-9a-f]{1,4}$/i.test(text))
      throw new Error("address must be 0000-FFFF");
    return parseInt(text, 16);
  }

  function parseLength(value) {
    const length = Number(value);
    if (!Number.isInteger(length) || length < 1 || length > 256)
      throw new Error("length must be 1-256 bytes");
    return length;
  }

  function parseBytes(value, maximum = 256) {
    const text = String(value || "").trim();
    if (!text) throw new Error("enter at least one byte");
    const tokens = text.split(/[\s,]+/).filter(Boolean);
    if (tokens.length > maximum)
      throw new Error("memory write exceeds " + maximum + " bytes");
    return tokens.map(token => {
      const hex = token.replace(/^0x/i, "").replace(/^&/, "");
      if (!/^[0-9a-f]{1,2}$/i.test(hex))
        throw new Error("invalid byte: " + token);
      return parseInt(hex, 16);
    });
  }

  function hex(value, width) {
    return (Number(value) >>> 0).toString(16).toUpperCase().padStart(width, "0");
  }

  function formatMemory(address, bytes) {
    const lines = [];
    for (let offset = 0; offset < bytes.length; offset += 8) {
      const row = bytes.slice(offset, offset + 8);
      const hexBytes = row.map(value => hex(value, 2)).join(" ").padEnd(23, " ");
      const ascii = row.map(value => value >= 0x20 && value < 0x7f
        ? String.fromCharCode(value) : ".").join("");
      lines.push(hex((address + offset) & 0xffff, 4) + "  " + hexBytes + "  " + ascii);
    }
    return lines.join("\n");
  }

  function normalizeLabel(value) {
    const label = String(value || "").trim();
    if (!label) throw new Error("label is required");
    if (label.length > 32) throw new Error("label is limited to 32 characters");
    return label;
  }

  return { parseAddress, parseLength, parseBytes, formatMemory, normalizeLabel, hex };
}));
