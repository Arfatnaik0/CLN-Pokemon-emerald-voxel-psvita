#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
static uint32_t seed=0xbadbeefu;
static float sample(void){seed=seed*1664525u+1013904223u;return ((int32_t)(seed>>8)-0x800000)/8388608.f;}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static int scenario(FILE *f, int inrate, int outrate, int channels){
 SDL_AudioStream *s=SDL_NewAudioStream(AUDIO_F32SYS,channels,inrate,AUDIO_F32SYS,channels,outrate);if(!s)return fprintf(stderr,"stream: %s\n",SDL_GetError()),1;
 float *in=malloc(4096*channels*sizeof(float)),*out=malloc(8192*channels*sizeof(float));if(!in||!out)return 1;
 for(int n=0;n<1200;n++) {
  int frames=n%7==0?1:(n%7==1?511:(n%7==2?512:(n%7==3?701:(n%7==4?734:(n%7==5?2048:1+(seed%1024))))));
  for(int i=0;i<frames*channels;i++)in[i]=sample();
  if(n%71==0)SDL_AudioStreamClear(s);
  if(SDL_AudioStreamPut(s,in,frames*channels*sizeof(float)))return fprintf(stderr,"put %s\n",SDL_GetError()),1;
  if(n%97==0 && SDL_AudioStreamFlush(s))return fprintf(stderr,"flush %s\n",SDL_GetError()),1;
  int bytes=SDL_AudioStreamAvailable(s);if(bytes<0||bytes>8192*channels*(int)sizeof(float))return 1;
  int got=SDL_AudioStreamGet(s,out,bytes);if(got!=bytes)return 1;
  fwrite(&bytes,sizeof(bytes),1,f);fwrite(out,1,bytes,f);
 }
 free(in);free(out);SDL_FreeAudioStream(s);return 0;
}
int main(int argc,char **argv){if(argc<2)return 1;FILE *f=fopen(argv[1],"wb");if(!f)return 1;
 if(scenario(f,42060,44100,2)||scenario(f,42060,44100,1)||scenario(f,44100,48000,2))return 1;fclose(f);
 SDL_AudioStream *s=SDL_NewAudioStream(AUDIO_F32SYS,2,42060,AUDIO_F32SYS,2,44100);float in[701*2],out[1500*2];for(int i=0;i<701*2;i++)in[i]=sample();
 double start=now();for(int i=0;i<4000;i++){if(i%500==0)SDL_AudioStreamClear(s);if(SDL_AudioStreamPut(s,in,sizeof(in)))return 1;int avail=SDL_AudioStreamAvailable(s);if(avail<0||avail>(int)sizeof(out)||SDL_AudioStreamGet(s,out,avail)!=avail)return 1;}printf("4000 streaming blocks: %.3f ms\n",(now()-start)*1000);SDL_FreeAudioStream(s);return 0;}
