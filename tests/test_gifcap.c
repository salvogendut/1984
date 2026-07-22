#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gifcap.h"

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static const uint8_t *find_gce(const uint8_t *data, size_t len) {
    static const uint8_t marker[] = { 0x21, 0xF9, 0x04 };
    for (size_t i = 0; i + 8 <= len; i++) {
        if (memcmp(data + i, marker, sizeof(marker)) == 0)
            return data + i;
    }
    return NULL;
}

int main(void) {
    const int out_w = 192;
    const int out_h = 144;
    const int delay_cs = 20;
    uint32_t pixels[8] = {
        0x00000000, 0x00FFFFFF, 0x00000000, 0x00FFFFFF,
        0x00FFFFFF, 0x00000000, 0x00FFFFFF, 0x00000000,
    };

    GifCap *cap = gifcap_open_mem(4, 2, out_w, out_h, delay_cs);
    assert(cap != NULL);

    size_t len = 0;
    const uint8_t *gif = gifcap_encode_single(cap, pixels, &len);
    assert(gif != NULL);
    assert(len > 16);
    assert(memcmp(gif, "GIF89a", 6) == 0);
    assert(read_le16(gif + 6) == out_w);
    assert(read_le16(gif + 8) == out_h);

    const uint8_t *gce = find_gce(gif, len);
    assert(gce != NULL);
    assert(read_le16(gce + 4) == delay_cs);
    assert(gifcap_frame_count(cap) == 1);

    gifcap_close(cap);
    puts("gifcap tests passed");
    return 0;
}
