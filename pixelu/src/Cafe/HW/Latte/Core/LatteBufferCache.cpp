// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "util/ChunkedHeap/ChunkedHeap.h"
#include "util/helpers/fspinlock.h"
#include "config/ActiveSettings.h"

// GPU side cache of guest memory used as vertex/uniform/index data.
//
// Guest memory is mirrored in disjoint, page aligned ranges. Each range owns a
// slice of one big host buffer (allocated from a VHeap). Changes made by the CPU
// are found by hashing pages; pages written by GPU streamout are filled with a
// signature pattern in guest RAM so that the cache can tell guest writes from
// its own data and never overwrite streamout results with stale RAM contents.

namespace
{
	constexpr uint32 kPageSize = 0x400;
	constexpr uint32 kPageMask = kPageSize - 1;

	// 16 byte pattern written to guest RAM where streamout data only exists in the cache
	constexpr uint64 kStreamoutFill0 = 0xF0F0F0F0155C5B6Aull;
	constexpr uint64 kStreamoutFill1 = 0x8BE6336411814F4Full;

	inline uint32 PageFloor(uint32 addr) { return addr & ~kPageMask; }
	inline uint32 PageCeil(uint32 addr) { return (addr + kPageMask) & ~kPageMask; }

	// Bumped when the whole cache should be rechecked against RAM (per swap, if enabled)
	uint32 s_checkEpoch = 0;

	std::unique_ptr<VHeap> s_heap;
	std::vector<uint8> s_uploadStaging;
	// heap offsets of destroyed ranges, released once the current draw is submitted
	std::vector<uint32> s_pendingFrees;

	uint64 PageHash(const uint8* page)
	{
		// four independent multiply/rotate lanes over the page
		uint64 lane[4] = { 0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull };
		constexpr uint64 kMul[4] = { 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0xD6E8FEB86659FD93ull };
		for (uint32 off = 0; off < kPageSize; off += 32)
		{
			for (int i = 0; i < 4; i++)
			{
				uint64 v;
				memcpy(&v, page + off + i * 8, 8);
				lane[i] = std::rotl(lane[i] ^ (v * kMul[i]), 29) * 0x94D049BB133111EBull;
			}
		}
		return lane[0] ^ std::rotl(lane[1], 16) ^ std::rotl(lane[2], 32) ^ std::rotl(lane[3], 48);
	}

	uint64 FullStreamoutPageHash()
	{
		alignas(16) uint8 page[kPageSize];
		for (uint32 i = 0; i < kPageSize; i += 16)
		{
			memcpy(page + i, &kStreamoutFill0, 8);
			memcpy(page + i + 8, &kStreamoutFill1, 8);
		}
		return PageHash(page);
	}

	bool IsStreamoutFill(const uint8* block16)
	{
		uint64 a, b;
		memcpy(&a, block16, 8);
		memcpy(&b, block16 + 8, 8);
		return a == kStreamoutFill0 && b == kStreamoutFill1;
	}
}

class CachedRange;
static void LatteBufferCache_unlink(CachedRange* range);

// every live range, in an order that only changes by swap-removal
static std::vector<CachedRange*> s_rangeList;

class CachedRange
{
public:
	static CachedRange* Create(MPTR begin, MPTR end, std::span<CachedRange*> replaced);

	~CachedRange()
	{
		if (m_hasAllocation)
			s_pendingFrees.emplace_back(m_heapOffset);
		CachedRange* last = s_rangeList.back();
		s_rangeList.pop_back();
		if (last != this)
		{
			s_rangeList[m_listIndex] = last;
			last->m_listIndex = m_listIndex;
		}
	}

	MPTR Begin() const { return m_begin; }
	MPTR End() const { return m_end; }

	uint32 HostOffset(MPTR addr) const
	{
		cemu_assert_debug(m_hasAllocation && addr >= m_begin && addr < m_end);
		return m_heapOffset + (addr - m_begin);
	}

	void MarkUsed()
	{
		m_usedInDraw = LatteGPUState.drawCallCounter;
		m_usedInFrame = LatteGPUState.frameCounter;
	}
	bool UsedByCurrentDraw() const { return m_usedInDraw == LatteGPUState.drawCallCounter; }
	uint32 FramesUnused() const { return LatteGPUState.frameCounter - m_usedInFrame; }
	bool HasStreamoutData() const { return m_hasStreamout; }
	// without streamout data the whole range can be rebuilt from RAM
	bool Restorable() const { return !m_hasStreamout; }

	void FreeAllocationNow()
	{
		if (!m_hasAllocation)
			return;
		s_heap->freeOffset(m_heapOffset);
		m_hasAllocation = false;
	}

	// Brings the cache up to date before [addr, addr+size) is used by a draw
	void Refresh(MPTR addr, uint32 size)
	{
		if (m_checkedEpoch != s_checkEpoch)
		{
			m_checkedEpoch = s_checkEpoch;
			SyncPages(m_begin, m_end, true);
			m_hasDirty = false;
		}
		if (!m_hasDirty)
			return;
		// Only one dirty interval is tracked. Pages are synced where the requested
		// range and the dirty interval meet when that trims the interval from one
		// side; otherwise the whole interval is synced.
		const uint32 wantBegin = PageFloor(addr);
		const uint32 wantEnd = PageCeil(addr + size);
		const uint32 b = std::max(m_dirtyBegin, wantBegin);
		const uint32 e = std::min(wantEnd, m_dirtyEnd);
		if (b >= e)
			return;
		const bool touchesFront = (b == m_dirtyBegin);
		const bool touchesBack = (e == m_dirtyEnd);
		if (touchesFront)
		{
			m_dirtyBegin = e;
			SyncPages(b, e, true);
			if (touchesBack)
				m_dirtyEnd = b;
			else
			{
				SyncPages(m_dirtyBegin, m_dirtyEnd, true);
				m_dirtyBegin = m_dirtyEnd;
			}
		}
		else if (touchesBack)
		{
			m_dirtyEnd = b;
			SyncPages(b, e, true);
		}
		else
		{
			SyncPages(m_dirtyBegin, m_dirtyEnd, true);
			m_dirtyBegin = m_dirtyEnd;
		}
		if (m_dirtyEnd <= m_dirtyBegin)
			m_hasDirty = false;
	}

	// widens the dirty interval to cover [begin, end) (clipped to this range, page aligned)
	void MarkDirty(MPTR begin, MPTR end)
	{
		begin = std::max(begin, m_begin);
		end = std::min(end, m_end);
		if (begin >= end)
			return;
		if (m_hasDirty)
		{
			m_dirtyBegin = std::min(m_dirtyBegin, begin);
			m_dirtyEnd = std::max(m_dirtyEnd, end);
		}
		else
		{
			m_dirtyBegin = begin;
			m_dirtyEnd = end;
			m_hasDirty = true;
		}
		m_dirtyBegin = PageFloor(m_dirtyBegin);
		m_dirtyEnd = PageCeil(m_dirtyEnd);
	}

	// GPU streamout wrote [begin, end): fill the guest copy with the signature
	void NoteStreamoutWrite(MPTR begin, MPTR end)
	{
		// only whole 16 byte blocks are tracked
		if (begin & 0xF)
		{
			cemuLog_logDebugOnce(LogType::Force, "Buffer cache: streamout write start {:08x} (end {:08x}) is not 16 byte aligned", begin, end);
			begin = (begin + 0xF) & ~0xF;
			end = std::max(begin, end);
		}
		end &= ~0xF;
		begin = std::max(begin, m_begin);
		end = std::min(end, m_end);
		if (begin >= end)
			return;
		const uint32 firstPage = PageIndex(PageFloor(begin));
		const uint32 pageCount = (PageCeil(end) - PageFloor(begin)) / kPageSize;
		for (uint32 i = 0; i < pageCount; i++)
			FillPageWithStreamoutSignature(firstPage + i, begin, end);
		if (pageCount > 0)
			m_hasStreamout = true;
	}

	static void DestroyUnusedRestorable(MPTR keepBegin, MPTR keepEnd);

private:
	struct PageState
	{
		uint64 hash{ 0 };
		bool streamout{ false };
	};

	CachedRange(MPTR begin, MPTR end) : m_begin(begin), m_end(end)
	{
		cemu_assert_debug(begin < end);
		MarkUsed();
		m_pages.resize((end - begin) / kPageSize);
		m_listIndex = (uint32)s_rangeList.size();
		s_rangeList.emplace_back(this);
	}

	uint32 PageIndex(MPTR alignedAddr) const
	{
		return (alignedAddr - m_begin) / kPageSize;
	}

	bool Allocate()
	{
		cemu_assert_debug(!m_hasAllocation);
		m_hasAllocation = s_heap->allocOffset(m_end - m_begin, kPageSize, m_heapOffset);
		return m_hasAllocation;
	}

	// Compares the pages of [begin, end) against their stored hashes. Changed pages
	// are uploaded in runs; pages holding streamout data are handled one by one
	// so that the signature blocks are never uploaded.
	void SyncPages(MPTR begin, MPTR end, bool upload)
	{
		cemu_assert_debug(begin >= m_begin && end <= m_end && begin < m_end);
		cemu_assert_debug(((begin | end) & kPageMask) == 0);
		const uint32 first = PageIndex(begin);
		const uint32 count = (end - begin) / kPageSize;
		const uint8* mem = memory_getPointerFromPhysicalOffset(begin);
		sint32 runStart = -1;
		auto flushRun = [&](uint32 runEnd)
		{
			if (runStart >= 0 && upload)
				UploadPages((uint32)runStart, runEnd);
			runStart = -1;
		};
		for (uint32 i = 0; i < count; i++, mem += kPageSize)
		{
			PageState& page = m_pages[first + i];
			const uint32 pageIndex = first + i;
			if (page.streamout)
			{
				flushRun(pageIndex);
				const uint64 h = PageHash(mem);
				if (h != page.hash)
				{
					page.hash = h;
					if (!UploadPageSkippingStreamout(pageIndex))
						page.streamout = false; // the CPU replaced all of it
				}
				continue;
			}
			const uint64 h = PageHash(mem);
			if (h != page.hash)
			{
				page.hash = h;
				if (runStart < 0)
					runStart = (sint32)pageIndex;
			}
			else
				flushRun(pageIndex);
		}
		flushRun(first + count);
	}

	// uploads pages [first, last) from a snapshot of RAM so the hashes match what was uploaded
	void UploadPages(uint32 first, uint32 last)
	{
		cemu_assert_debug(last > first);
		const uint32 bytes = (last - first) * kPageSize;
		const MPTR addr = m_begin + first * kPageSize;
		if (s_uploadStaging.size() < bytes)
			s_uploadStaging.resize(bytes);
		memcpy(s_uploadStaging.data(), memory_getPointerFromPhysicalOffset(addr), bytes);
		for (uint32 i = 0; i < last - first; i++)
			m_pages[first + i].hash = PageHash(s_uploadStaging.data() + i * kPageSize);
		g_renderer->bufferCache_upload(s_uploadStaging.data(), bytes, HostOffset(addr));
	}

	// uploads the parts of one page that are not streamout signature blocks;
	// returns whether the page still contains signature blocks
	bool UploadPageSkippingStreamout(uint32 pageIndex)
	{
		alignas(16) uint8 snapshot[kPageSize];
		const MPTR pageAddr = m_begin + pageIndex * kPageSize;
		memcpy(snapshot, memory_getPointerFromPhysicalOffset(pageAddr), kPageSize);
		m_pages[pageIndex].hash = PageHash(snapshot);
		bool anyFill = false;
		sint32 runStart = -1;
		auto flushRun = [&](uint32 runEnd)
		{
			if (runStart >= 0)
			{
				const uint32 from = (uint32)runStart * 16;
				g_renderer->bufferCache_upload(snapshot + from, runEnd * 16 - from, HostOffset(pageAddr + from));
			}
			runStart = -1;
		};
		for (uint32 block = 0; block < kPageSize / 16; block++)
		{
			if (IsStreamoutFill(snapshot + block * 16))
			{
				anyFill = true;
				flushRun(block);
			}
			else if (runStart < 0)
				runStart = (sint32)block;
		}
		flushRun(kPageSize / 16);
		return anyFill;
	}

	void FillPageWithStreamoutSignature(uint32 pageIndex, MPTR begin, MPTR end)
	{
		const MPTR pageBegin = m_begin + pageIndex * kPageSize;
		const MPTR pageEnd = pageBegin + kPageSize;
		begin = std::max(begin, pageBegin);
		end = std::min(end, pageEnd);
		cemu_assert_debug(begin < end && ((begin | end) & 0xF) == 0);
		uint8* mem = memory_getPointerFromPhysicalOffset(begin);
		for (MPTR a = begin; a < end; a += 16, mem += 16)
		{
			memcpy(mem, &kStreamoutFill0, 8);
			memcpy(mem + 8, &kStreamoutFill1, 8);
		}
		PageState& page = m_pages[pageIndex];
		page.streamout = true;
		// A fully overwritten page has a known hash, so the fill itself does not
		// look like a CPU write. A partial fill forces the page to be rechecked.
		static const uint64 s_fullFillHash = FullStreamoutPageHash();
		page.hash = (begin == pageBegin && end == pageEnd) ? s_fullFillHash : 0;
	}

	// whole range from RAM (after creation)
	void LoadFromRAM()
	{
		cemu_assert_debug(m_hasAllocation);
		SyncPages(m_begin, m_end, false); // only records the page hashes
		g_renderer->bufferCache_upload(memory_getPointerFromPhysicalOffset(m_begin), m_end - m_begin, HostOffset(m_begin));
	}

	// takes over the cached contents (and page state) of an overlapping old range
	void TakeOverFrom(const CachedRange* src)
	{
		const MPTR b = std::max(m_begin, src->m_begin);
		const MPTR e = std::min(m_end, src->m_end);
		cemu_assert_debug(b < e);
		g_renderer->bufferCache_copy(src->HostOffset(b), HostOffset(b), e - b);
		const uint32 count = (e - b) / kPageSize;
		const PageState* from = src->m_pages.data() + src->PageIndex(b);
		PageState* to = m_pages.data() + PageIndex(b);
		for (uint32 i = 0; i < count; i++)
		{
			to[i] = from[i];
			m_hasStreamout |= from[i].streamout;
		}
	}

	MPTR m_begin;
	MPTR m_end; // exclusive
	bool m_hasAllocation{ false };
	uint32 m_heapOffset{ 0 };
	uint32 m_usedInDraw{};
	uint32 m_usedInFrame{};
	uint32 m_listIndex{};
	uint32 m_checkedEpoch{ s_checkEpoch - 1 };
	std::vector<PageState> m_pages;
	bool m_hasStreamout{ false };
	bool m_hasDirty{ false };
	MPTR m_dirtyBegin{};
	MPTR m_dirtyEnd{};
};

// Drops ranges that are not used by the current draw, hold no streamout data and do
// not overlap [keepBegin, keepEnd)
void CachedRange::DestroyUnusedRestorable(MPTR keepBegin, MPTR keepEnd)
{
	size_t i = 0;
	while (i < s_rangeList.size())
	{
		CachedRange* r = s_rangeList[i];
		const bool overlapsKept = r->m_begin < keepEnd && r->m_end > keepBegin;
		if (r->UsedByCurrentDraw() || !r->Restorable() || overlapsKept)
		{
			i++;
			continue;
		}
		r->FreeAllocationNow();
		LatteBufferCache_unlink(r);
		delete r; // moves another range into slot i
	}
}

// New range covering [begin, end). Data of the replaced (already unlinked) ranges is
// carried over and the replaced ranges are destroyed.
CachedRange* CachedRange::Create(MPTR begin, MPTR end, std::span<CachedRange*> replaced)
{
	CachedRange* range = new CachedRange(begin, end);
	if (!range->Allocate())
	{
		// first give up replaced ranges whose contents can be reloaded from RAM
		for (auto& old : replaced)
		{
			if (old->UsedByCurrentDraw() || !old->Restorable())
				continue;
			old->FreeAllocationNow();
			delete old;
			old = nullptr;
		}
		if (!range->Allocate())
		{
			cemuLog_log(LogType::Force, "GPU buffer cache is full ({}KB requested), evicting unused ranges", (end - begin + 1023) / 1024);
			DestroyUnusedRestorable(begin, end);
			if (!range->Allocate())
			{
				cemuLog_log(LogType::Force, "GPU buffer cache: could not free enough space");
				cemu_assert(false);
			}
		}
	}
	range->LoadFromRAM();
	for (CachedRange* old : replaced)
	{
		if (!old)
			continue;
		range->TakeOverFrom(old);
		delete old;
	}
	return range;
}

// --- range lookup: disjoint ranges keyed by start address ------------------

static std::map<MPTR, CachedRange*> s_rangeMap;
static std::vector<CachedRange*> s_queryResult;

static CachedRange* LatteBufferCache_findContaining(MPTR addr)
{
	auto it = s_rangeMap.upper_bound(addr);
	if (it == s_rangeMap.begin())
		return nullptr;
	--it;
	return addr < it->second->End() ? it->second : nullptr;
}

// all ranges intersecting [begin, end), in address order
static void LatteBufferCache_findOverlapping(MPTR begin, MPTR end, std::vector<CachedRange*>& out)
{
	out.clear();
	if (begin >= end || s_rangeMap.empty())
		return;
	auto it = s_rangeMap.upper_bound(begin);
	if (it != s_rangeMap.begin())
		--it;
	for (; it != s_rangeMap.end() && it->first < end; ++it)
	{
		if (it->second->End() > begin)
			out.emplace_back(it->second);
	}
}

static void LatteBufferCache_link(CachedRange* range)
{
	cemu_assert_debug(!s_rangeMap.contains(range->Begin()));
	s_rangeMap.emplace(range->Begin(), range);
}

static void LatteBufferCache_unlink(CachedRange* range)
{
	auto it = s_rangeMap.find(range->Begin());
	cemu_assert(it != s_rangeMap.end() && it->second == range);
	s_rangeMap.erase(it);
}

// range that fully contains [addr, addr+size), merging and replacing ranges as needed
static CachedRange* LatteBufferCache_acquire(MPTR addr, uint32 size)
{
	CachedRange* hit = LatteBufferCache_findContaining(addr);
	if (hit && addr >= hit->Begin() && addr + size <= hit->End())
		return hit;
	MPTR begin = PageFloor(addr);
	MPTR end = PageCeil(addr + size);
	LatteBufferCache_findOverlapping(begin, end, s_queryResult);
	if (!s_queryResult.empty())
	{
		begin = std::min<uint32>(begin, s_queryResult.front()->Begin());
		end = std::max<uint32>(end, s_queryResult.back()->End());
		for (CachedRange* r : s_queryResult)
			LatteBufferCache_unlink(r);
	}
	CachedRange* range = CachedRange::Create(begin, end, s_queryResult);
	LatteBufferCache_link(range);
	return range;
}

// --- public interface ------------------------------------------------------

uint32 LatteBufferCache_retrieveDataInCache(MPTR physAddress, uint32 size)
{
	CachedRange* range = LatteBufferCache_acquire(physAddress, size);
	range->MarkUsed();
	range->Refresh(physAddress, size);
	return range->HostOffset(physAddress);
}

void LatteBufferCache_copyStreamoutDataToCache(MPTR physAddress, uint32 size, uint32 streamoutBufferOffset)
{
	if (size == 0)
		return;
	cemu_assert_debug(size >= 16);
	CachedRange* range = LatteBufferCache_acquire(physAddress, size);
	range->MarkUsed();
	g_renderer->bufferCache_copyStreamoutToMainBuffer(streamoutBufferOffset, range->HostOffset(physAddress), size);
	range->NoteStreamoutWrite(physAddress, physAddress + size);
}

void LatteBufferCache_invalidate(MPTR physAddress, uint32 size)
{
	if (size == 0 || physAddress >= 0xFFFFF000 || physAddress + size < physAddress)
		return;
	LatteBufferCache_findOverlapping(physAddress, physAddress + size, s_queryResult);
	for (CachedRange* r : s_queryResult)
		r->MarkDirty(physAddress, physAddress + size);
}

static void LatteBufferCache_invalidatePage(MPTR pageAddr)
{
	cemu_assert_debug((pageAddr & kPageMask) == 0);
	if (CachedRange* r = LatteBufferCache_findContaining(pageAddr))
		r->MarkDirty(pageAddr, pageAddr + kPageSize);
}

void LatteBufferCache_processDeallocations()
{
	for (uint32 offset : s_pendingFrees)
		s_heap->freeOffset(offset);
	s_pendingFrees.clear();
}

void LatteBufferCache_init(size_t bufferSize)
{
	cemu_assert_debug(s_rangeMap.empty());
	s_heap = std::make_unique<VHeap>(nullptr, (uint32)bufferSize);
	g_renderer->bufferCache_init((uint32)bufferSize);
}

void LatteBufferCache_UnloadAll()
{
	while (!s_rangeList.empty())
	{
		CachedRange* r = s_rangeList.front();
		r->FreeAllocationNow();
		LatteBufferCache_unlink(r);
		delete r;
	}
	s_pendingFrees.clear();
}

void LatteBufferCache_getStats(uint32& heapSize, uint32& allocationSize, uint32& allocNum)
{
	s_heap->getStats(heapSize, allocationSize, allocNum);
}

// --- data cache flushes from the CPU side ----------------------------------

// Pages named by DCFlushRange are collected by the CPU threads and turned into
// invalidations on the GPU thread.
static FSpinlock s_flushLock;
static std::vector<uint32> s_flushedPages;
static std::vector<uint32> s_flushedPagesWork;
static std::atomic<bool> s_flushPending{ false };

void LatteBufferCache_notifyDCFlush(MPTR address, uint32 size)
{
	if (address == 0 || size == 0xFFFFFFFF)
		return; // whole-cache flushes are not tracked
	const uint32 firstPage = address / kPageSize;
	const uint32 lastPage = (address + size - 1) / kPageSize;
	s_flushLock.lock();
	for (uint32 p = firstPage; p <= lastPage; p++)
		s_flushedPages.emplace_back(p);
	s_flushPending.store(true, std::memory_order_release);
	s_flushLock.unlock();
}

void LatteBufferCache_processDCFlushQueue()
{
	if (!s_flushPending.load(std::memory_order_acquire))
		return;
	s_flushLock.lock();
	std::swap(s_flushedPages, s_flushedPagesWork);
	s_flushPending.store(false, std::memory_order_relaxed);
	s_flushLock.unlock();
	std::sort(s_flushedPagesWork.begin(), s_flushedPagesWork.end());
	auto last = std::unique(s_flushedPagesWork.begin(), s_flushedPagesWork.end());
	for (auto it = s_flushedPagesWork.begin(); it != last; ++it)
		LatteBufferCache_invalidatePage(*it * kPageSize);
	s_flushedPagesWork.clear();
}

void LatteBufferCache_notifySwapTVScanBuffer()
{
	if (ActiveSettings::FlushGPUCacheOnSwap())
		s_checkEpoch++;
}

// Looks at one range per call (round robin) and evicts it when it has not been
// used for long enough. The fuller the heap, the shorter the allowed idle time.
void LatteBufferCache_incrementalCleanup()
{
	static uint32 s_cursor = 0;
	if (s_rangeList.empty())
		return;
	s_cursor = (s_cursor + 1) % (uint32)s_rangeList.size();
	CachedRange* r = s_rangeList[s_cursor];
	// streamout results only exist in the cache, keep them while they may still be read
	if (r->HasStreamoutData() && r->FramesUnused() < 120)
		return;
	uint32 heapSize, usedSize, allocCount;
	s_heap->getStats(heapSize, usedSize, allocCount);
	uint32 maxIdleFrames;
	if (usedSize >= heapSize * 4 / 5)
		maxIdleFrames = 2;
	else if (usedSize >= heapSize * 3 / 4)
		maxIdleFrames = 4;
	else if (usedSize >= heapSize / 2)
		maxIdleFrames = 20;
	else
		maxIdleFrames = 500;
	if (r->FramesUnused() >= maxIdleFrames)
	{
		LatteBufferCache_unlink(r);
		delete r;
	}
}
