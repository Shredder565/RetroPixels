#include "wiiu.h"
// Coroutines: two of them taking turns, and the locked cache.
typedef struct { uint32_t w[0x180 / 4]; } __attribute__((aligned(8))) OSCoroutine;
void OSInitCoroutine(OSCoroutine *co, void (*entry)(OSCoroutine *, int), void *stack);
void OSSwitchCoroutine(OSCoroutine *from, OSCoroutine *to);
void *LCAlloc(uint32_t size);
void LCDealloc(void *p);
uint32_t LCGetAllocatableSize(void);
int LCEnableDMA(void);
void LCLoadDMABlocks(void *lc, const void *mem, uint32_t blocks);
void LCStoreDMABlocks(void *mem, const void *lc, uint32_t blocks);

static OSCoroutine mainCo, workCo;
static uint8_t stack[0x4000] __attribute__((aligned(16)));
static volatile int steps;
static OSCoroutine *entrySelf;
static int entryArg;

// OSSwitchCoroutine starts it with (to, 1)
static void worker(OSCoroutine *self, int one) {
	entrySelf = self;
	entryArg = one;
	for (;;) {
		steps += 10;
		OSSwitchCoroutine(&workCo, &mainCo);
	}
}

int main(int argc, char **argv) {
	OSInitCoroutine(&workCo, worker, stack + sizeof(stack) - 16);
	int local = 7;
	for (int i = 0; i < 3; i++) {
		steps++;
		OSSwitchCoroutine(&mainCo, &workCo);
	}
	OSReport("co: steps %d local %d\n", steps, local);
	// mainCo's r31 field: where it switched to
	OSReport("co: entry %d %d r31 %d\n", entrySelf == &workCo, entryArg, mainCo.w[(0x18 + 17 * 4) / 4] == (uint32_t)&workCo);

	uint32_t before = LCGetAllocatableSize();
	uint32_t *lc = LCAlloc(0x1000);
	LCEnableDMA();
	uint32_t src[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, back[16] = {0};
	LCLoadDMABlocks(lc, src, 2);
	lc[0] = 100;
	LCStoreDMABlocks(back, lc, 2);
	uint32_t during = LCGetAllocatableSize();
	LCDealloc(lc);
	OSReport("lc: %x %x %x %d %d\n", before, during, LCGetAllocatableSize(), back[0], back[15]);
	return 0;
}
