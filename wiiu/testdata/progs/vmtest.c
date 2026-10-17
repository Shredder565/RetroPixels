#include "wiiu.h"
// Virtual memory mapping and DMA copies.
void OSGetMapVirtAddrRange(uint32_t *start, uint32_t *size);
uint32_t OSAllocVirtAddr(uint32_t va, uint32_t size, uint32_t align);
int OSMapMemory(uint32_t va, uint32_t pa, uint32_t size, int perm);
uint32_t OSQueryVirtAddr(uint32_t va);
uint32_t OSEffectiveToPhysical(uint32_t ea);
uint64_t DMAECopyMem(void *dst, const void *src, uint32_t words, int endian);
uint64_t DMAEFillMem(void *dst, uint32_t value, uint32_t words);
int DMAEWaitDone(uint64_t ts);

int main(int argc, char **argv) {
	uint32_t start = 0, size = 0;
	OSGetMapVirtAddrRange(&start, &size);
	uint32_t *buf = MEMAllocFromDefaultHeapEx(0x40000, 0x20000);
	buf[0] = 0x12345678;
	uint32_t va = OSAllocVirtAddr(0, 0x20000, 0x20000);
	int ok = OSMapMemory(va, OSEffectiveToPhysical((uint32_t)buf), 0x20000, 3);
	volatile uint32_t *alias = (uint32_t *)va;
	uint32_t seen = alias[0];
	alias[1] = 0xcafe;
	OSReport("vm: range %x+%x va %x map %d seen %x back %x query %d\n", start, size, va, ok, seen, buf[1],
		OSQueryVirtAddr(va + 8) == OSEffectiveToPhysical((uint32_t)buf) + 8);

	uint32_t src[4] = {0x11223344, 0x55667788, 1, 2}, dst[4] = {0};
	DMAEWaitDone(DMAECopyMem(dst, src, 2, 2));
	DMAEWaitDone(DMAEFillMem(dst + 2, 0xabcd, 2));
	OSReport("dmae: %x %x %x %x\n", dst[0], dst[1], dst[2], dst[3]);
	return 0;
}
