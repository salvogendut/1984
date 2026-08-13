(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  root.JS1984Media = api;
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  function resolveHttpUrl(value, baseUrl, parameter) {
    if (!value) return null;
    let url;
    try {
      url = new URL(value, baseUrl);
    } catch (_) {
      throw new Error(parameter + ' is not a valid URL');
    }
    if (url.protocol !== 'http:' && url.protocol !== 'https:')
      throw new Error(parameter + ' must use HTTP or HTTPS');
    return url.href;
  }

  function validateAutorun(value) {
    if (!value) return null;
    if (value.length > 240 || /[\x00-\x1f\x7f"]/.test(value))
      throw new Error('autorun contains unsupported characters');
    return value;
  }

  function validateMemory(value) {
    if (value === null) return null;
    if (!/^(128|256|512|1024)$/.test(value))
      throw new Error('memory must be 128, 256, 512, or 1024');
    return Number(value);
  }

  function parseStartupMedia(search, baseUrl) {
    const params = new URLSearchParams(search || '');
    /* `disk` remains a compatibility alias for links published before drive B
     * support. The canonical parameter is now `diska`. */
    const diskAParameter = params.has('diska') ? 'diska' : 'disk';
    const media = {
      diskA: resolveHttpUrl(params.get(diskAParameter), baseUrl, diskAParameter),
      diskB: resolveHttpUrl(params.get('diskb'), baseUrl, 'diskb'),
      cartridge: resolveHttpUrl(params.get('cartridge'), baseUrl, 'cartridge'),
      autorun: validateAutorun(params.get('autorun')),
      memoryKb: validateMemory(params.get('memory')),
    };
    if (media.autorun && !media.diskA)
      throw new Error('autorun requires a diska URL');
    return media;
  }

  function filenameFromUrl(value, fallback) {
    if (!value) return fallback;
    const path = new URL(value).pathname;
    const encoded = path.slice(path.lastIndexOf('/') + 1);
    if (!encoded) return fallback;
    try {
      return decodeURIComponent(encoded);
    } catch (_) {
      return encoded;
    }
  }

  return { parseStartupMedia, filenameFromUrl };
}));
