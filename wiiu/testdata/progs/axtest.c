/* RetroPixels' sound test: plays a looping 16-bit square wave on the TV's
   left speaker and a short DSP ADPCM sound on its right, and counts AX
   frame callbacks. ax_test.go checks the sound. */
#include "wiiu.h"

typedef struct { uint32_t renderer, unk, pipeline; } AXInitParams;
typedef struct { uint16_t format, loop; uint32_t loopOffset, endOffset, currentOffset; const void *data; } AXVoiceOffsets;
typedef struct { uint16_t volume; int16_t delta; } AXVoiceVeData;
typedef struct { uint16_t volume; int16_t delta; } AXBus;
typedef struct { AXBus bus[4]; } AXChannelMix;
typedef struct { uint16_t coefs[16]; uint16_t gain, predScale; int16_t prev[2]; } AXVoiceAdpcm;
typedef void *AXVoice;
void AXInitWithParams(AXInitParams *p);
AXVoice AXAcquireVoice(uint32_t priority, void *callback, void *user);
void AXVoiceBegin(AXVoice v);
void AXVoiceEnd(AXVoice v);
void AXSetVoiceOffsets(AXVoice v, AXVoiceOffsets *o);
void AXSetVoiceAdpcm(AXVoice v, AXVoiceAdpcm *a);
void AXSetVoiceSrcRatio(AXVoice v, float ratio);
void AXSetVoiceVe(AXVoice v, AXVoiceVeData *ve);
int AXSetVoiceDeviceMix(AXVoice v, int device, uint32_t id, AXChannelMix *mix);
void AXSetVoiceState(AXVoice v, uint16_t state);
BOOL AXIsVoiceRunning(AXVoice v);
uint32_t AXGetVoiceLoopCount(AXVoice v);
void *AXRegisterFrameCallback(void (*cb)(void));

static int16_t square[96];
static uint8_t adpcm[8] __attribute__((aligned(8))) = {0x0b, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
static volatile int frames;
static void onFrame(void) { frames++; }

static void mixTo(AXVoice v, int ch, uint16_t vol) {
	AXChannelMix mix[6];
	memset(mix, 0, sizeof mix);
	mix[ch].bus[0].volume = vol;
	AXSetVoiceDeviceMix(v, 0, 0, mix);
}

int main(int argc, char **argv) {
	AXInitParams p = {1, 0, 0}; /* 48 kHz */
	AXInitWithParams(&p);
	AXRegisterFrameCallback(onFrame);
	for (int i = 0; i < 96; i++)
		square[i] = i < 48 ? 0x4000 : -0x4000;

	AXVoice a = AXAcquireVoice(10, 0, 0);
	AXVoiceBegin(a);
	AXVoiceOffsets oa = {0x0a, 1, 0, 95, 0, square};
	AXSetVoiceOffsets(a, &oa);
	AXSetVoiceSrcRatio(a, 1.0f);
	AXVoiceVeData ve = {0x8000, 0};
	AXSetVoiceVe(a, &ve);
	mixTo(a, 0, 0x8000);
	AXSetVoiceState(a, 1);
	AXVoiceEnd(a);

	AXVoice b = AXAcquireVoice(10, 0, 0);
	AXVoiceOffsets ob = {0x00, 0, 0, 15, 2, adpcm};
	AXSetVoiceOffsets(b, &ob);
	AXVoiceAdpcm ad;
	memset(&ad, 0, sizeof ad);
	ad.predScale = 0x0b;
	AXSetVoiceAdpcm(b, &ad);
	AXSetVoiceSrcRatio(b, 1.0f);
	AXSetVoiceVe(b, &ve);
	mixTo(b, 1, 0x4000);
	AXSetVoiceState(b, 1);

	OSSleepTicks(248625000 / 4 / 10); /* 100 ms */
	OSReport("frames %d running %d %d loops %d\n", frames, AXIsVoiceRunning(a), AXIsVoiceRunning(b), AXGetVoiceLoopCount(a));
	OSReport("ax test done\n");
	for (;;)
		OSSleepTicks(248625000 / 4 / 60);
}
