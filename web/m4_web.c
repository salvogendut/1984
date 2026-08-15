/* EM_JS boundary between the M4 emulation (src/m4.c) and the browser bridge
 * (m4-bridge.js, installed on globalThis.JS1984M4Bridge).
 *
 * Every function reads or writes the bridge's current state synchronously;
 * the WebSocket relay I/O happens between emulator frames. HEAPU8 is used to
 * copy guest buffers in and out of wasm memory.
 */
#include "m4_web.h"

#include <emscripten.h>

EM_JS(int, m4_web_connect, (int channel, const uint8_t *ip, uint16_t port), {
    const bridge = globalThis.JS1984M4Bridge;
    if (!bridge) return -1;
    return bridge.tcpConnect(channel, HEAPU8.subarray(ip, ip + 4), port);
});

EM_JS(int, m4_web_poll, (int channel), {
    const bridge = globalThis.JS1984M4Bridge;
    return bridge ? bridge.poll(channel) : 0;
});

EM_JS(int, m4_web_avail, (int channel), {
    const bridge = globalThis.JS1984M4Bridge;
    return bridge ? bridge.avail(channel) : 0;
});

EM_JS(int, m4_web_send, (int channel, const uint8_t *data, size_t length), {
    const bridge = globalThis.JS1984M4Bridge;
    return bridge && bridge.send(channel, HEAPU8.subarray(data, data + length))
        ? 0 : -1;
});

EM_JS(int, m4_web_recv, (int channel, uint8_t *data, size_t maxlen), {
    const bridge = globalThis.JS1984M4Bridge;
    return bridge ? bridge.recv(channel, HEAPU8, data, maxlen) : -2;
});

EM_JS(void, m4_web_close, (int channel), {
    const bridge = globalThis.JS1984M4Bridge;
    if (bridge) bridge.close(channel);
});

EM_JS(int, m4_web_dns, (const char *host), {
    const bridge = globalThis.JS1984M4Bridge;
    return bridge && bridge.dns(UTF8ToString(host)) ? 1 : 0;
});

EM_JS(int, m4_web_dns_poll, (uint8_t *out4), {
    const bridge = globalThis.JS1984M4Bridge;
    return bridge ? bridge.dnsPoll(HEAPU8, out4) : 0;
});
