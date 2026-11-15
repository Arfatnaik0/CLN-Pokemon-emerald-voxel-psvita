#include "global.h"
#include "cgb_audio.h"
#include "cgb_tables.h"

struct AudioCGB gb;
float soundChannelPos[4];
const s16 *PU1Table;
const s16 *PU2Table;
u32 apuFrame;
u8 apuCycle;
u32 sampleRate;
u16 lfsrMax[2];
float ch4Samples;

#ifndef CGB_AUDIO_DISABLE_PHASE_STEP_CACHE
struct CgbPhaseStepCache
{
    u16 frequency;
    float step;
    u8 valid;
};

static struct CgbPhaseStepCache sTonePhaseStepCache[3];
static struct CgbPhaseStepCache sNoisePhaseStepCache;
#endif

static inline float GetTonePhaseStep(u8 channel, u16 frequency)
{
#ifdef CGB_AUDIO_DISABLE_PHASE_STEP_CACHE
    (void)channel;
    return freqTable[frequency] / (sampleRate / 32);
#else
    struct CgbPhaseStepCache *cache = &sTonePhaseStepCache[channel];
    if (!cache->valid || cache->frequency != frequency)
    {
        cache->frequency = frequency;
        cache->step = freqTable[frequency] / (sampleRate / 32);
        cache->valid = TRUE;
    }
    return cache->step;
#endif
}

static inline float GetNoisePhaseStep(u8 frequency)
{
#ifdef CGB_AUDIO_DISABLE_PHASE_STEP_CACHE
    return freqTableNSE[frequency] / sampleRate;
#else
    struct CgbPhaseStepCache *cache = &sNoisePhaseStepCache;
    if (!cache->valid || cache->frequency != frequency)
    {
        cache->frequency = frequency;
        cache->step = freqTableNSE[frequency] / sampleRate;
        cache->valid = TRUE;
    }
    return cache->step;
#endif
}

void cgb_audio_init(u32 rate){
    gb.ch1Freq = 0;
    gb.ch1SweepCounter = 0;
    gb.ch1SweepCounterI = 0;
    gb.ch1SweepDir = 0;
    gb.ch1SweepShift = 0;
    for (u8 ch = 0; ch < 4; ch++){
        gb.Vol[ch] = 0;
        gb.VolI[ch] = 0;
        gb.Len[ch] = 0;
        gb.LenI[ch] = 0;
        gb.LenOn[ch] = 0;
        gb.EnvCounter[ch] = 0;
        gb.EnvCounterI[ch] = 0;
        gb.EnvDir[ch] = 0;
        gb.DAC[ch] = 0;
        soundChannelPos[ch] = 0;
    }
    soundChannelPos[1] = 1;
#ifndef CGB_AUDIO_DISABLE_PHASE_STEP_CACHE
    for (u8 ch = 0; ch < 3; ch++)
        sTonePhaseStepCache[ch].valid = FALSE;
    sNoisePhaseStepCache.valid = FALSE;
#endif
    PU1Table = PU0;
    PU2Table = PU0;
    sampleRate = rate;
    gb.ch4LFSR[0] = 0x8000;
    gb.ch4LFSR[1] = 0x80;
    lfsrMax[0] = 0x8000;
    lfsrMax[1] = 0x80;
    ch4Samples = 0.0f;
}


void cgb_set_sweep(u8 sweep){
    gb.ch1SweepDir = (sweep & 0x08) >> 3;
    gb.ch1SweepCounter = gb.ch1SweepCounterI = (sweep & 0x70) >> 4;
    gb.ch1SweepShift = (sweep & 0x07);
}


void cgb_set_wavram(){
    for(u8 wavi = 0; wavi < 0x10; wavi++){
        gb.WAVRAM[(wavi << 1)] = (((*(REG_ADDR_WAVE_RAM0 + wavi)) & 0xF0) >> 4) / 7.5f - 1.0f;
        gb.WAVRAM[(wavi << 1) + 1] = (((*(REG_ADDR_WAVE_RAM0 + wavi)) & 0x0F)) / 7.5f - 1.0f;
    }
}


void cgb_toggle_length(u8 channel, bool8 state){
    gb.LenOn[channel] = state;
}


void cgb_set_length(u8 channel, u8 length){
    gb.Len[channel] = gb.LenI[channel] = length;
}


void cgb_set_envelope(u8 channel, u8 envelope){
    if(channel == 2){
        switch((envelope & 0xE0)){
            case 0x00:  // mute
                gb.Vol[2] = gb.VolI[2] = 0;
            break;
            case 0x20:  // full
                gb.Vol[2] = gb.VolI[2] = 4;
            break;
            case 0x40:  // half
                gb.Vol[2] = gb.VolI[2] = 2;
            break;
            case 0x60:  // quarter
                gb.Vol[2] = gb.VolI[2] = 1;
            break;
            case 0x80:  // 3 quarters
                gb.Vol[2] = gb.VolI[2] = 3;
            break;
        }
    }else{
        gb.DAC[channel] = (envelope & 0xF8) > 0;
        gb.Vol[channel] = gb.VolI[channel] = (envelope & 0xF0) >> 4;
        gb.EnvDir[channel] = (envelope & 0x08) >> 3;
        gb.EnvCounter[channel] = gb.EnvCounterI[channel] = (envelope & 0x07);
    }
}


void cgb_trigger_note(u8 channel){
    gb.Vol[channel] = gb.VolI[channel];
    gb.Len[channel] = gb.LenI[channel];
    if(channel != 2) gb.EnvCounter[channel] = gb.EnvCounterI[channel];
    if(channel == 3) {
        gb.ch4LFSR[0] = 0x8000;
        gb.ch4LFSR[1] = 0x80;
    }
}


void cgb_audio_generate(u16 samplesPerFrame){
    float *outBuffer = gb.outBuffer;
    switch(REG_NR11 & 0xC0){
        case 0x00:
            PU1Table = PU0;
        break;
        case 0x40:
            PU1Table = PU1;
        break;
        case 0x80:
            PU1Table = PU2;
        break;
        case 0xC0:
            PU1Table = PU3;
        break;
    }

    switch(REG_NR21 & 0xC0){
        case 0x00:
            PU2Table = PU0;
        break;
        case 0x40:
            PU2Table = PU1;
        break;
        case 0x80:
            PU2Table = PU2;
        break;
        case 0xC0:
            PU2Table = PU3;
        break;
    }

    float step0 = GetTonePhaseStep(0, REG_SOUND1CNT_X & 0x7FF);
    const float step1 = GetTonePhaseStep(1, REG_SOUND2CNT_H & 0x7FF);
    const float step2 = GetTonePhaseStep(2, REG_SOUND3CNT_X & 0x7FF);
    const float noiseStep = GetNoisePhaseStep(REG_SOUND4CNT_H & 0xFF);
    const u8 nr51 = REG_NR51;
    const bool8 ch3Dac = (REG_NR30 & 0x80) != 0;

    for (u16 i = 0; i < samplesPerFrame; i++, outBuffer+=2) {
        apuFrame += 512;
        if(__builtin_expect(apuFrame >= sampleRate, 0)){
            apuFrame -= sampleRate;
            apuCycle++;

            if((apuCycle & 1) == 0){  // Length
                for(u8 ch = 0; ch < 4; ch++){
                    if(gb.Len[ch]){
                        if(--gb.Len[ch] == 0 && gb.LenOn[ch]){
                            REG_NR52 &= (0xFF ^ (1 << ch));
                        }
                    }
                }
            }

            if((apuCycle & 7) == 7){  // Envelope
                for(u8 ch = 0; ch < 4; ch++){
                    if(ch == 2) continue;  // Skip wave channel
                    if(gb.EnvCounter[ch]){
                        if(--gb.EnvCounter[ch] == 0){
                            if(gb.Vol[ch] && !gb.EnvDir[ch]){
                                gb.Vol[ch]--;
                                gb.EnvCounter[ch] = gb.EnvCounterI[ch];
                            }else if(gb.Vol[ch] < 0x0F && gb.EnvDir[ch]){
                                gb.Vol[ch]++;
                                gb.EnvCounter[ch] = gb.EnvCounterI[ch];
                            }
                        }
                    }
                }
            }

            if((apuCycle & 3) == 2){  // Sweep
                if(gb.ch1SweepCounterI && gb.ch1SweepShift){
                    if(--gb.ch1SweepCounter == 0){
                        gb.ch1Freq = REG_SOUND1CNT_X & 0x7FF;
                        if(gb.ch1SweepDir){
                            gb.ch1Freq -= gb.ch1Freq >> gb.ch1SweepShift;
                            if(gb.ch1Freq & 0xF800) gb.ch1Freq = 0;
                        }else{
                            gb.ch1Freq += gb.ch1Freq >> gb.ch1SweepShift;
                            if(gb.ch1Freq & 0xF800){
                                gb.ch1Freq = 0;
                                gb.EnvCounter[0] = 0;
                                gb.Vol[0] = 0;
                            }
                        }
                        REG_NR13 = gb.ch1Freq & 0xFF;
                        REG_NR14 &= 0xF8;
                        REG_NR14 += (gb.ch1Freq >> 8) & 0x07;
                        gb.ch1SweepCounter = gb.ch1SweepCounterI;
                        step0 = GetTonePhaseStep(0, REG_SOUND1CNT_X & 0x7FF);
                    }
                }
            }
        }
        //Sound generation loop
        soundChannelPos[0] += step0;
        soundChannelPos[1] += step1;
        soundChannelPos[2] += step2;
        if (__builtin_expect(soundChannelPos[0] >= 32, 0)) {
            do { soundChannelPos[0] -= 32; } while (soundChannelPos[0] >= 32);
        }
        if (__builtin_expect(soundChannelPos[1] >= 32, 0)) {
            do { soundChannelPos[1] -= 32; } while (soundChannelPos[1] >= 32);
        }
        if (__builtin_expect(soundChannelPos[2] >= 32, 0)) {
            do { soundChannelPos[2] -= 32; } while (soundChannelPos[2] >= 32);
        }
        float outputL = 0;
        float outputR = 0;
        u8 nr52 = REG_NR52;
        if(nr52 & 0x80){
            if((gb.DAC[0]) && (nr52 & 0x01) && (nr51 & 0x11) && gb.Vol[0]){
                float s0 = gb.Vol[0] * PU1Table[(int)(soundChannelPos[0])] / 15.0f;
                if(nr51 & 0x10) outputL += s0;
                if(nr51 & 0x01) outputR += s0;
            }
            if((gb.DAC[1]) && (nr52 & 0x02) && (nr51 & 0x22) && gb.Vol[1]){
                float s1 = gb.Vol[1] * PU2Table[(int)(soundChannelPos[1])] / 15.0f;
                if(nr51 & 0x20) outputL += s1;
                if(nr51 & 0x02) outputR += s1;
            }
            if(ch3Dac && (nr52 & 0x04) && (nr51 & 0x44) && gb.Vol[2]){
                float s2 = gb.Vol[2] * gb.WAVRAM[(int)(soundChannelPos[2])] / 4.0f;
                if(nr51 & 0x40) outputL += s2;
                if(nr51 & 0x04) outputR += s2;
            }
            if((gb.DAC[3]) && (nr52 & 0x08)){
                bool32 lfsrMode = ((REG_NR43 & 0x08) == 8);
                ch4Samples += noiseStep;
                int ch4Out = (gb.ch4LFSR[lfsrMode] & 1) ? 1 : -1;
                int avgDiv = 1;
                while(ch4Samples >= 1){
                    avgDiv++;
                    bool8 lfsrCarry = 0;
                    if(gb.ch4LFSR[lfsrMode] & 2) lfsrCarry ^= 1;
                    gb.ch4LFSR[lfsrMode] >>= 1;
                    if(gb.ch4LFSR[lfsrMode] & 2) lfsrCarry ^= 1;
                    if(lfsrCarry) gb.ch4LFSR[lfsrMode] |= lfsrMax[lfsrMode];
                    ch4Out += (gb.ch4LFSR[lfsrMode] & 1) ? 1 : -1;
                    ch4Samples--;
                }
                if ((nr51 & 0x88) && gb.Vol[3]) {
                    float sample = (float)ch4Out;
                    if(avgDiv > 1) sample /= avgDiv;
                    float s3 = gb.Vol[3] * sample / 15.0f;
                    if(nr51 & 0x80) outputL += s3;
                    if(nr51 & 0x08) outputR += s3;
                }
            }
        }
        outBuffer[0] = outputL * 0.25f;
        outBuffer[1] = outputR * 0.25f;
    }
}


float *cgb_get_buffer(){
    return gb.outBuffer;
}
