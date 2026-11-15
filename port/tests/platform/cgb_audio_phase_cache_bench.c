#include "global.h"
#include "cgb_audio.h"
#include <time.h>

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
static volatile float sSink;

static void Setup(void)
{
    memset(REG_BASE, 0, sizeof(REG_BASE));
    cgb_audio_init(42060);
    REG_NR52 = 0x8F;
    REG_NR51 = 0xFF;
    REG_NR11 = 0x80;
    REG_NR21 = 0x40;
    REG_NR30 = 0x80;
    REG_NR43 = 0x03;
    for (u8 i = 0; i < 16; i++)
        REG_BASE[REG_OFFSET_WAVE_RAM0 + i] = (u8)(i * 17 + 3);
    cgb_set_wavram();
    cgb_set_envelope(0, 0xF0);
    cgb_set_envelope(1, 0xA8);
    cgb_set_envelope(2, 0x80);
    cgb_set_envelope(3, 0xD8);
    for (u8 ch = 0; ch < 4; ch++)
    {
        cgb_set_length(ch, 200);
        cgb_toggle_length(ch, FALSE);
        cgb_trigger_note(ch);
    }
    REG_SOUND1CNT_X = 0x8000 | 512;
    REG_SOUND2CNT_H = 1327;
    REG_SOUND3CNT_X = 0x8000 | 1999;
    REG_SOUND4CNT_H = 67;
}

int main(int argc, char **argv)
{
    const u32 frames = 1200;
    const int dynamic = argc > 1;
    Setup();
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (u32 frame = 0; frame < frames; frame++)
    {
        if (dynamic)
        {
            REG_SOUND1CNT_X = 0x8000 | ((512 + frame * 7) & 0x7FF);
            REG_SOUND2CNT_H = (1327 + frame * 13) & 0x7FF;
            REG_SOUND3CNT_X = 0x8000 | ((1999 + frame * 17) & 0x7FF);
            REG_SOUND4CNT_H = (u8)(67 + frame * 19);
        }
        cgb_audio_generate(701);
        sSink += cgb_get_buffer()[frame % 1402];
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsedMs = (end.tv_sec - start.tv_sec) * 1000.0
                     + (end.tv_nsec - start.tv_nsec) / 1000000.0;
    printf("%.3f ms, sink=%.5f\n", elapsedMs, sSink);
    return 0;
}
