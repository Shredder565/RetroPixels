/* an OSScreen homebrew program, the way simple Wii U homebrew is written */
#include "wiiu.h"
int main(int argc, char **argv) {
	ProcUIInit(0);
	OSScreenInit();
	uint32_t tvSize = OSScreenGetBufferSizeEx(0), drcSize = OSScreenGetBufferSizeEx(1);
	void *mem1 = MEMGetBaseHeapHandle(0);
	uint8_t *tv = MEMAllocFromFrmHeapEx(mem1, tvSize, 0x100);
	uint8_t *drc = MEMAllocFromFrmHeapEx(mem1, drcSize, 0x100);
	OSScreenSetBufferEx(0, tv);
	OSScreenSetBufferEx(1, drc);
	OSScreenEnableEx(0, 1);
	OSScreenEnableEx(1, 1);
	int frame = 0, presses = 0;
	while (ProcUIProcessMessages(1) == 0) {
		VPADStatus st;
		int32_t err;
		if (VPADRead(0, &st, 1, &err) == 1 && (st.trigger & 0x8000)) presses++;
		char line[80];
		for (int s = 0; s < 2; s++) {
			OSScreenClearBufferEx(s, 0x10204000);
			OSScreenPutFontEx(s, 0, 0, "Hello from the Wii U!");
			__os_snprintf(line, sizeof line, "frame %d  A presses %d", frame, presses);
			OSScreenPutFontEx(s, 0, 2, line);
			for (int x = 0; x < 100; x++) OSScreenPutPixelEx(s, 300 + x, 300, 0xff0000ff);
		}
		DCFlushRange(tv, tvSize);
		DCFlushRange(drc, drcSize);
		OSScreenFlipBuffersEx(0);
		OSScreenFlipBuffersEx(1);
		frame++;
		if (presses >= 2) break;
		OSSleepTicks(248625000 / 4 / 60);
	}
	OSReport("done after %d frames\n", frame);
	return 0;
}
