#include "SDL.h"
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#define NTHREAD 8
static pthread_barrier_t barrier;
struct ctx {int index; uint64_t hash; int failure;};
static void *worker(void *v){struct ctx *c=v;uint32_t seed=12345+(uint32_t)c->index;float input[701*2],output[800*2];SDL_AudioStream *s=SDL_NewAudioStream(AUDIO_F32SYS,2,42060,AUDIO_F32SYS,2,44100);if(!s){c->failure=1;return NULL;}pthread_barrier_wait(&barrier);uint64_t hash=1469598103934665603ull;for(int n=0;n<300;n++){for(int i=0;i<701*2;i++){seed=seed*1664525u+1013904223u;input[i]=((int32_t)(seed>>8)-0x800000)/8388608.f;}if(n%79==0)SDL_AudioStreamClear(s);if(SDL_AudioStreamPut(s,input,sizeof(input))){c->failure=2;break;}int bytes=SDL_AudioStreamAvailable(s);if(bytes<0||bytes>(int)sizeof(output)||SDL_AudioStreamGet(s,output,bytes)!=bytes){c->failure=3;break;}const uint8_t *b=(const uint8_t *)output;for(int i=0;i<bytes;i++){hash^=b[i];hash*=1099511628211ull;}}c->hash=hash;SDL_FreeAudioStream(s);return NULL;}
int main(void){struct ctx c[NTHREAD]={0};pthread_t t[NTHREAD];pthread_barrier_init(&barrier,NULL,NTHREAD);for(int i=0;i<NTHREAD;i++){c[i].index=i;if(pthread_create(&t[i],NULL,worker,&c[i]))return 1;}for(int i=0;i<NTHREAD;i++){pthread_join(t[i],NULL);if(c[i].failure)return 1;printf("%016llx\n",(unsigned long long)c[i].hash);}pthread_barrier_destroy(&barrier);return 0;}
