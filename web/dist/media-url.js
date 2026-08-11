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

  function parseStartupMedia(search, baseUrl) {
    const params = new URLSearchParams(search || '');
    const media = {
      disk: resolveHttpUrl(params.get('disk'), baseUrl, 'disk'),
      cartridge: resolveHttpUrl(params.get('cartridge'), baseUrl, 'cartridge'),
      autorun: validateAutorun(params.get('autorun')),
    };
    if (media.autorun && !media.disk)
      throw new Error('autorun requires a disk URL');
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
