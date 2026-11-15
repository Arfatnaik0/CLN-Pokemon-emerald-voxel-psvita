#include "global.h"
#include "cgb_audio.h"

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));

static int SetupAudio(u32 rate)
{
    memset(REG_BASE, 0, sizeof(REG_BASE));
    cgb_audio_init(rate);
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
    cgb_set_sweep(0x12);
    for (u8 ch = 0; ch < 4; ch++)
    {
        cgb_set_length(ch, 200);
        cgb_toggle_length(ch, FALSE);
        cgb_trigger_note(ch);
    }
    return 0;
}

static int Run(u32 rate, u32 samples, u8 pattern)
{
    if (SetupAudio(rate) != 0)
        return 1;

    for (u32 i = 0; i < samples; i++)
    {
        u16 f1, f2, f3;
        u8 fn;
        if (pattern == 0)
        {
            f1 = 512;
            f2 = 1327;
            f3 = 1999;
            fn = 67;
        }
        else if (pattern == 1)
        {
            f1 = (u16)((i * 71 + (i >> 3) * 13) & 0x7FF);
            f2 = (u16)((i * 127 + (i >> 2) * 29) & 0x7FF);
            f3 = (u16)((i * 191 + (i >> 1) * 7) & 0x7FF);
            fn = (u8)(i * 17 + (i >> 4) * 9);
        }
        else
        {
            f1 = (u16)(((i / 31) * 37 + 5) & 0x7FF);
            f2 = (u16)(((i / 47) * 89 + 2) & 0x7FF);
            f3 = (u16)(((i / 19) * 131 + 7) & 0x7FF);
            fn = (u8)((i / 13) * 11 + 3);
        }
        REG_SOUND1CNT_X = 0x8000 | f1;
        REG_SOUND2CNT_H = f2;
        REG_SOUND3CNT_X = 0x8000 | f3;
        REG_SOUND4CNT_H = fn;
        cgb_audio_generate(1);
        if (fwrite(cgb_get_buffer(), sizeof(float), 2, stdout) != 2)
            return 1;
    }
    return 0;
}

static int WriteFixedFrequencySamples(u32 samples)
{
    REG_SOUND1CNT_X = 0x8000 | 512;
    REG_SOUND2CNT_H = 1327;
    REG_SOUND3CNT_X = 0x8000 | 1999;
    REG_SOUND4CNT_H = 67;
    for (u32 i = 0; i < samples; i++)
    {
        cgb_audio_generate(1);
        if (fwrite(cgb_get_buffer(), sizeof(float), 2, stdout) != 2)
            return 1;
    }
    return 0;
}

int main(void)
{
    /* Rates include both Vita rates, reset cases, and small nonzero divisors. */
    if (Run(42060, 2048, 0) != 0
     || Run(42060, 2048, 1) != 0
     || Run(42048, 2048, 2) != 0
     || Run(44100, 2048, 1) != 0
     || Run(32, 4, 0) != 0
     || Run(33, 4, 1) != 0
     || SetupAudio(42060) != 0
     || WriteFixedFrequencySamples(128) != 0
     || SetupAudio(44100) != 0
     || WriteFixedFrequencySamples(128) != 0)
        return 1;
    return 0;
}
