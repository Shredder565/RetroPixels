/* RetroPixels' Wii U demo: a homebrew program (written for RetroPixels, built
   with clang) that shows the GamePad's buttons and sticks on the TV and the
   GamePad. It comes in the download as "RetroPixels Wii U demo.rpx", so there's
   something to try before you have homebrew of your own. Plus + Minus ends it. */
#include "wiiu.h"

static const char *names[] = {"Sync", "HOME", "-", "+", "R", "L", "ZR", "ZL", "Down", "Up", "Right", "Left",
	"Y", "X", "B", "A", "TV", "R-click", "L-click"};

static void box(int s, int x0, int y0, int size, uint32_t col) {
	for (int i = 0; i <= size; i++) {
		OSScreenPutPixelEx(s, x0 + i, y0, col);
		OSScreenPutPixelEx(s, x0 + i, y0 + size, col);
		OSScreenPutPixelEx(s, x0, y0 + i, col);
		OSScreenPutPixelEx(s, x0 + size, y0 + i, col);
	}
}

static void fill(int s, int x0, int y0, int w, int h, uint32_t col) {
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) OSScreenPutPixelEx(s, x0 + x, y0 + y, col);
}

static void stick(int s, int x0, int y0, float x, float y) {
	box(s, x0, y0, 120, 0x8090a0ff);
	int px = x0 + 60 + (int)(x * 54), py = y0 + 60 - (int)(y * 54);
	fill(s, px - 5, py - 5, 11, 11, 0x40e060ff);
}

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
	int frame = 0, bx = 40, by = 40, dx = 3, dy = 2;
	VPADStatus st;
	memset(&st, 0, sizeof st);
	while (ProcUIProcessMessages(1) == 0) {
		int32_t err;
		VPADRead(0, &st, 1, &err);
		if ((st.hold & 0xc) == 0xc) break; /* Plus + Minus */
		char line[200];
		for (int s = 0; s < 2; s++) {
			int w = s ? 854 : 1280, h = s ? 480 : 720;
			OSScreenClearBufferEx(s, 0x101828ff);
			OSScreenPutFontEx(s, 0, 0, "RetroPixels Wii U demo");
			OSScreenPutFontEx(s, 0, 1, s ? "This is the GamePad's screen." : "This is the TV.");
			int n = __os_snprintf(line, sizeof line, "Held:");
			for (int b = 0; b < 19; b++)
				if (st.hold & (1u << b)) n += __os_snprintf(line + n, sizeof line - n, " %s", names[b]);
			OSScreenPutFontEx(s, 0, 3, line);
			__os_snprintf(line, sizeof line, "Left stick  %d, %d    Right stick  %d, %d", (int)(st.lx * 100), (int)(st.ly * 100),
				(int)(st.rx * 100), (int)(st.ry * 100));
			OSScreenPutFontEx(s, 0, 4, line);
			__os_snprintf(line, sizeof line, "Frame %d", frame);
			OSScreenPutFontEx(s, 0, 5, line);
			OSScreenPutFontEx(s, 0, h / 24 - 2, "Plus + Minus ends the demo.");
			stick(s, 40, 170, st.lx, st.ly);
			stick(s, 200, 170, st.rx, st.ry);
			int sx = bx * (w - 60) / 1220, sy = 300 + by * (h - 360) / 360;
			fill(s, sx, sy, 40, 40, (st.hold & 0x8000) ? 0xe04040ff : 0x4080e0ff);
		}
		DCFlushRange(tv, tvSize);
		DCFlushRange(drc, drcSize);
		OSScreenFlipBuffersEx(0);
		OSScreenFlipBuffersEx(1);
		bx += dx; by += dy;
		if (bx < 0 || bx > 1220) dx = -dx;
		if (by < 0 || by > 360) dy = -dy;
		frame++;
		OSSleepTicks(248625000 / 4 / 60);
	}
	OSReport("demo ended after %d frames\n", frame);
	return 0;
}
