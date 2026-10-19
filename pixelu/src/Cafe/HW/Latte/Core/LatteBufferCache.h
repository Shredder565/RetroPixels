// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Host-side cache of guest memory used as vertex, uniform and streamout buffers

void LatteBufferCache_init(size_t bufferSize);
void LatteBufferCache_UnloadAll();

// makes sure the range is in the cache and up to date, returns its offset in the cache buffer
uint32 LatteBufferCache_retrieveDataInCache(MPTR physAddress, uint32 size);
void LatteBufferCache_copyStreamoutDataToCache(MPTR physAddress, uint32 size, uint32 streamoutBufferOffset);
void LatteBufferCache_invalidate(MPTR physAddress, uint32 size);

// CPU cache flushes by the guest mark ranges as modified
void LatteBufferCache_notifyDCFlush(MPTR address, uint32 size);
void LatteBufferCache_processDCFlushQueue();

void LatteBufferCache_processDeallocations();
void LatteBufferCache_incrementalCleanup();

void LatteBufferCache_getStats(uint32& heapSize, uint32& allocationSize, uint32& allocNum);

void LatteBufferCache_notifySwapTVScanBuffer();
