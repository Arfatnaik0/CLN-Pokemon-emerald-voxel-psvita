#include <stdint.h>
#include <stdio.h>
#include "platform/gba_rgba.h"

int main(void)
{
    static uint32_t table[GBA_RGBA_COLOR_COUNT];
    static uint16_t input[65536];
    static uint32_t output[65536];
    GbaBuildRgbaTable(table);
    for (unsigned int i = 0; i < 65536; i++) input[i] = (uint16_t)i;
    GbaConvertRgba(input, output, 65536, table);
    for (unsigned int i = 0; i < 65536; i++) {
        uint32_t r = (i & 31) * 255 / 31;
        uint32_t g = ((i >> 5) & 31) * 255 / 31;
        uint32_t b = ((i >> 10) & 31) * 255 / 31;
        uint32_t expected = r | (g << 8) | (b << 16) | 0xff000000u;
        if (output[i] != expected) {
            fprintf(stderr, "RGBA mismatch for %04x\n", i);
            return 1;
        }
    }
    puts("PASS: all 65536 GBA colors match original RGBA conversion");
    return 0;
}
