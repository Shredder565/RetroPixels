#include "wiiu.h"
// The MEM heaps: expanded, frame, unit and block heaps, the base heaps and
// the memory bounds, and __preinit_user.
void *MEMCreateExpHeapEx(void *start, uint32_t size, uint16_t flags);
uint32_t MEMResizeForMBlockExpHeap(void *heap, void *p, uint32_t size);
uint32_t MEMGetAllocatableSizeForExpHeapEx(void *heap, int align);
uint32_t MEMGetTotalFreeSizeForExpHeap(void *heap);
uint32_t MEMGetSizeForMBlockExpHeap(void *p);
void MEMVisitAllocatedForExpHeap(void *heap, void (*visit)(void *p, void *heap, uint32_t param), uint32_t param);
void *MEMFindContainHeap(void *p);
void *MEMFindParentHeap(void *heap);
void *MEMCreateFrmHeapEx(void *start, uint32_t size, uint16_t flags);
int MEMRecordStateForFrmHeap(void *heap, uint32_t id);
int MEMFreeByStateToFrmHeap(void *heap, uint32_t id);
uint32_t MEMGetAllocatableSizeForFrmHeapEx(void *heap, int align);
void *MEMCreateUnitHeapEx(void *start, uint32_t size, uint32_t block, int align, uint16_t flags);
void *MEMAllocFromUnitHeap(void *heap);
void MEMFreeToUnitHeap(void *heap, void *p);
uint32_t MEMCountFreeBlockForUnitHeap(void *heap);
uint32_t MEMCalcHeapSizeForUnitHeap(uint32_t block, uint32_t count, int align);
void *MEMInitBlockHeap(void *heap, void *start, void *end, void *tracks, uint32_t size, uint32_t flags);
void *MEMAllocFromBlockHeapEx(void *heap, uint32_t size, int align);
void MEMFreeToBlockHeap(void *heap, void *p);
uint32_t MEMGetTrackingLeftInBlockHeap(void *heap);
uint32_t MEMGetTotalFreeSizeForBlockHeap(void *heap);
uint32_t MEMGetAllocatableSizeForBlockHeapEx(void *heap, int align);
void OSGetMemBound(int type, uint32_t *start, uint32_t *size);
int OSGetForegroundBucket(uint32_t *start, uint32_t *size);
int OSGetForegroundBucketFreeArea(uint32_t *start, uint32_t *size);

static uint8_t expBuf[0x10000] __attribute__((aligned(0x100)));
static uint8_t frmBuf[0x1000] __attribute__((aligned(0x100)));
static uint8_t unitBuf[0x400] __attribute__((aligned(0x100)));
static uint8_t blockHdr[0x80] __attribute__((aligned(8)));
static uint8_t blockTracks[0x10 + 0x14 * 4] __attribute__((aligned(8)));
static uint8_t blockBuf[0x1000] __attribute__((aligned(0x100)));

static volatile int preinitCalls;
static volatile uint32_t preinitMEM1, preinitFG, preinitMEM2, preinitBase;

void __preinit_user(void **mem1, void **fg, void **mem2) {
	preinitCalls++;
	preinitMEM1 = (uint32_t)*mem1;
	preinitFG = (uint32_t)*fg;
	preinitMEM2 = (uint32_t)*mem2;
	preinitBase = (uint32_t)MEMGetBaseHeapHandle(1);
}

static uint32_t visited, visitedSize;
static void visit(void *p, void *heap, uint32_t param) {
	visited += param;
	visitedSize += MEMGetSizeForMBlockExpHeap(p);
}

#define OFF(p, base) ((uint32_t)(p) - (uint32_t)(base))

int main(int argc, char **argv) {
	// expanded heap
	void *h = MEMCreateExpHeapEx(expBuf, sizeof expBuf, 0);
	uint32_t free0 = MEMGetTotalFreeSizeForExpHeap(h);
	uint32_t al4 = MEMGetAllocatableSizeForExpHeapEx(h, 4), al40 = MEMGetAllocatableSizeForExpHeapEx(h, 0x40);
	void *a = MEMAllocFromExpHeapEx(h, 0x100, 4);
	void *b = MEMAllocFromExpHeapEx(h, 0x100, -0x100);
	uint32_t free1 = MEMGetTotalFreeSizeForExpHeap(h);
	void *c = MEMAllocFromExpHeapEx(h, 0x20, 0x40);
	uint32_t csize = MEMGetSizeForMBlockExpHeap(c);
	uint32_t shrunk = MEMResizeForMBlockExpHeap(h, a, 0x80);
	uint32_t grown = MEMResizeForMBlockExpHeap(h, a, 0xc0);
	uint32_t free2 = MEMGetTotalFreeSizeForExpHeap(h), al2 = MEMGetAllocatableSizeForExpHeapEx(h, 4);
	MEMVisitAllocatedForExpHeap(h, visit, 1);
	int contains = MEMFindContainHeap(c) == h;
	MEMFreeToExpHeap(h, c);
	uint32_t free3 = MEMGetTotalFreeSizeForExpHeap(h);
	MEMFreeToExpHeap(h, b);
	MEMFreeToExpHeap(h, a);
	OSReport("exp: %x %x %x | %x %x %x %x %x | %x %x %x %x | %d %x %d | %x %x\n", free0, al4, al40,
		OFF(a, h), OFF(b, h), free1, OFF(c, h), csize, shrunk, grown, free2, al2, visited, visitedSize, contains,
		free3, MEMGetTotalFreeSizeForExpHeap(h));

	// a heap inside the default heap
	void *base2 = MEMGetBaseHeapHandle(1);
	void *inner = MEMCreateExpHeapEx(MEMAllocFromExpHeapEx(base2, 0x1000, 4), 0x1000, 0);
	void *p = MEMAllocFromExpHeapEx(inner, 0x10, 4);
	OSReport("nested: %d %d %d\n", MEMFindContainHeap(p) == inner, MEMFindParentHeap(inner) == base2,
		MEMFindContainHeap(inner) == base2);

	// frame heap
	void *f = MEMCreateFrmHeapEx(frmBuf, sizeof frmBuf, 0);
	uint32_t fa = MEMGetAllocatableSizeForFrmHeapEx(f, 4), fb = MEMGetAllocatableSizeForFrmHeapEx(f, 0x100);
	void *x = MEMAllocFromFrmHeapEx(f, 0x10, 4);
	int rec = MEMRecordStateForFrmHeap(f, 1);
	void *y = MEMAllocFromFrmHeapEx(f, 0x100, 0x20);
	void *z = MEMAllocFromFrmHeapEx(f, 0x40, -0x40);
	uint32_t fc = MEMGetAllocatableSizeForFrmHeapEx(f, 4);
	int back = MEMFreeByStateToFrmHeap(f, 1);
	uint32_t fd = MEMGetAllocatableSizeForFrmHeapEx(f, 4);
	int again = MEMFreeByStateToFrmHeap(f, 1);
	MEMFreeToFrmHeap(f, 3);
	OSReport("frm: %x %x | %x %d %x %x %x | %d %x %d %x\n", fa, fb, OFF(x, f), rec, OFF(y, f), OFF(z, f), fc, back,
		fd, again, MEMGetAllocatableSizeForFrmHeapEx(f, 4));

	// unit heap
	void *u = MEMCreateUnitHeapEx(unitBuf, sizeof unitBuf, 0x30, 0x20, 0);
	uint32_t n0 = MEMCountFreeBlockForUnitHeap(u);
	void *u1 = MEMAllocFromUnitHeap(u), *u2 = MEMAllocFromUnitHeap(u);
	uint32_t n1 = MEMCountFreeBlockForUnitHeap(u);
	MEMFreeToUnitHeap(u, u1);
	void *u3 = MEMAllocFromUnitHeap(u);
	OSReport("unit: %d %x %x %d %x %x\n", n0, OFF(u1, u), OFF(u2, u), n1, OFF(u3, u), MEMCalcHeapSizeForUnitHeap(0x30, 14, 0x20));

	// block heap
	void *bh = MEMInitBlockHeap(blockHdr, blockBuf, blockBuf + sizeof blockBuf, blockTracks, sizeof blockTracks, 0);
	void *b1 = MEMAllocFromBlockHeapEx(bh, 0x100, 4);
	void *b2 = MEMAllocFromBlockHeapEx(bh, 0x100, -0x100);
	uint32_t left = MEMGetTrackingLeftInBlockHeap(bh), bfree = MEMGetTotalFreeSizeForBlockHeap(bh);
	uint32_t bal = MEMGetAllocatableSizeForBlockHeapEx(bh, 4);
	MEMFreeToBlockHeap(bh, b1);
	OSReport("block: %d %x %x %d %x %x | %d %x\n", bh == blockHdr, OFF(b1, blockBuf), OFF(b2, blockBuf), left, bfree, bal,
		MEMGetTrackingLeftInBlockHeap(bh), MEMGetTotalFreeSizeForBlockHeap(bh));

	// base heaps and bounds
	uint32_t m1, m1n, m2, m2n, fg, fgn, fga, fgan;
	OSGetMemBound(1, &m1, &m1n);
	OSGetMemBound(2, &m2, &m2n);
	OSGetForegroundBucket(&fg, &fgn);
	OSGetForegroundBucketFreeArea(&fga, &fgan);
	void *h1 = MEMGetBaseHeapHandle(0), *h8 = MEMGetBaseHeapHandle(8);
	OSReport("bounds: %x %x %x %x %x %x %x\n", m1, m1n, m2 + m2n, fg, fgn, fga, fgan);
	OSReport("base: %x %x %d %x %x\n", (uint32_t)h1, (uint32_t)h8, (uint32_t)base2 == m2,
		MEMGetAllocatableSizeForFrmHeapEx(h1, 4), MEMGetAllocatableSizeForFrmHeapEx(h8, 4));
	OSReport("preinit: %d %x %x %d %x\n", preinitCalls, preinitMEM1, preinitFG, preinitMEM2 == m2, preinitBase);
	return 0;
}
