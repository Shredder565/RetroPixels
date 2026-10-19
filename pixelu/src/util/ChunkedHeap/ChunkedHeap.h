// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <util/helpers/MemoryPool.h>

// Handle of a ChunkedHeap allocation. `internal` is the heap's bookkeeping
// record and is needed to free the allocation.
struct CHAddr
{
	uint32 offset;
	uint32 chunkIndex;
	void* internal;

	CHAddr(uint32 _offset, uint32 _chunkIndex, void* internal = nullptr) : offset(_offset), chunkIndex(_chunkIndex), internal(internal) {}
	CHAddr() : offset(0xFFFFFFFF), chunkIndex(0xFFFFFFFF), internal(nullptr) {}

	bool isValid() { return chunkIndex != 0xFFFFFFFF; }
	static CHAddr getInvalid() { return CHAddr(0xFFFFFFFF, 0xFFFFFFFF); }
};

namespace heap_detail
{
	// A block of address space, either free or handed out. Blocks of one
	// contiguous area are chained by address (prevAdj/nextAdj); free blocks are
	// additionally kept in size classes (floor(log2(size))), newest first.
	struct Block
	{
		Block* prevAdj{};
		Block* nextAdj{};
		Block* prevInClass{};
		Block* nextInClass{};
		uint32 offset;
		uint32 size;
		uint32 area; // chunk index (ChunkedHeap only)
		bool free;

		Block(uint32 offset, uint32 size, uint32 area, bool free) : offset(offset), size(size), area(area), free(free) {}
	};

	inline uint32 sizeClass(uint32 size)
	{
		return 31u - (uint32)std::countl_zero(size);
	}

	// offset in b where an allocation of size with the given alignment
	// (power of two) would start, if it fits
	inline bool fitsIn(const Block* b, uint32 size, uint32 alignment, uint32& startOut)
	{
		if (b->size < size)
			return false;
		const uint32 start = (b->offset + alignment - 1) & ~(alignment - 1);
		const uint32 padding = start - b->offset;
		if (padding > b->size - size)
			return false;
		startOut = start;
		return true;
	}

	// Free blocks sorted into 32 size classes plus the shared logic for
	// carving an allocation out of a block and for coalescing on release.
	// TPool provides Block* create(offset, size, area, free) and destroy(Block*).
	template<typename TPool>
	class FreeBlockIndex
	{
	public:
		Block* first(uint32 cls) const { return m_classes[cls]; }

		// bit n is set when class n has at least one block
		uint32 nonEmptyMask() const { return m_nonEmpty; }

		void insert(Block* b)
		{
			cemu_assert_debug(b->size != 0);
			const uint32 cls = sizeClass(b->size);
			Block* oldFirst = m_classes[cls];
			b->prevInClass = nullptr;
			b->nextInClass = oldFirst;
			if (oldFirst)
				oldFirst->prevInClass = b;
			m_classes[cls] = b;
			m_nonEmpty |= 1u << cls;
		}

		void remove(Block* b)
		{
			const uint32 cls = sizeClass(b->size);
			if (b->nextInClass)
				b->nextInClass->prevInClass = b->prevInClass;
			if (b->prevInClass)
			{
				b->prevInClass->nextInClass = b->nextInClass;
			}
			else
			{
				cemu_assert_debug(m_classes[cls] == b);
				m_classes[cls] = b->nextInClass;
				if (!m_classes[cls])
					m_nonEmpty &= ~(1u << cls);
			}
			b->prevInClass = b->nextInClass = nullptr;
		}

		// Turns the free block b into a used block [start, start + size). The
		// padding before it and the rest after it become free blocks of their
		// own (inserted in that order).
		void carve(TPool& pool, Block* b, uint32 start, uint32 size)
		{
			remove(b);
			const uint32 blockEnd = b->offset + b->size;
			const uint32 allocEnd = start + size;
			if (start > b->offset)
			{
				Block* before = pool.create(b->offset, start - b->offset, b->area, true);
				before->prevAdj = b->prevAdj;
				before->nextAdj = b;
				if (b->prevAdj)
					b->prevAdj->nextAdj = before;
				b->prevAdj = before;
				insert(before);
			}
			if (allocEnd < blockEnd)
			{
				Block* after = pool.create(allocEnd, blockEnd - allocEnd, b->area, true);
				after->prevAdj = b;
				after->nextAdj = b->nextAdj;
				if (b->nextAdj)
					b->nextAdj->prevAdj = after;
				b->nextAdj = after;
				insert(after);
			}
			b->offset = start;
			b->size = size;
			b->free = false;
		}

		// Marks the used block b free and merges it with free neighbours. When
		// the left neighbour is free it absorbs the others, otherwise a free
		// right neighbour absorbs b.
		void release(TPool& pool, Block* b)
		{
			Block* left = b->prevAdj;
			Block* right = b->nextAdj;
			const bool mergeLeft = left && left->free;
			const bool mergeRight = right && right->free;
			if (mergeLeft)
			{
				Block* last = b;
				if (mergeRight)
				{
					remove(right);
					last = right;
				}
				remove(left);
				left->size = (last->offset + last->size) - left->offset;
				unlink(last, left);
				insert(left);
				if (mergeRight)
					pool.destroy(right);
				pool.destroy(b);
			}
			else if (mergeRight)
			{
				remove(right);
				right->size = (right->offset + right->size) - b->offset;
				right->offset = b->offset;
				right->prevAdj = b->prevAdj;
				if (b->prevAdj)
					b->prevAdj->nextAdj = right;
				insert(right);
				pool.destroy(b);
			}
			else
			{
				b->free = true;
				insert(b);
			}
		}

	private:
		// drops every block between keep (exclusive) and lastRemoved (inclusive) from the address chain
		static void unlink(Block* lastRemoved, Block* keep)
		{
			keep->nextAdj = lastRemoved->nextAdj;
			if (lastRemoved->nextAdj)
				lastRemoved->nextAdj->prevAdj = keep;
		}

		Block* m_classes[32]{};
		uint32 m_nonEmpty{ 0 };
	};

	struct PooledBlocks
	{
		MemoryPool<Block> pool{ 64 };
		Block* create(uint32 offset, uint32 size, uint32 area, bool free) { return pool.allocObj(offset, size, area, free); }
		void destroy(Block* b) { pool.freeObj(b); }
	};

	struct HeapBlocks
	{
		Block* create(uint32 offset, uint32 size, uint32 area, bool free) { return new Block(offset, size, area, free); }
		void destroy(Block* b) { delete b; }
	};
}

// Heap over a growing set of chunks (e.g. GPU memory blocks). When no free
// block fits, the subclass is asked for a new chunk via allocateNewChunk(),
// which returns the chunk's size (a multiple of TMinimumAlignment, below 2GB)
// or 0 if no more memory can be obtained. After the first refusal the heap
// stops asking.
template<uint32 TMinimumAlignment = 32>
class ChunkedHeap
{
	using Block = heap_detail::Block;

public:
	ChunkedHeap() = default;

	CHAddr alloc(uint32 size, uint32 alignment = 4)
	{
		cemu_assert_debug(size <= (0x7FFFFFFFu - TMinimumAlignment));
		size = (size == 0) ? TMinimumAlignment : ((size + TMinimumAlignment - 1) & ~(TMinimumAlignment - 1));
		while (true)
		{
			uint32 start = 0;
			if (Block* b = findFit(size, alignment, start))
			{
				m_free.carve(m_blocks, b, start, size);
				m_numAllocatedBytes += size;
				return CHAddr(start, b->area, b);
			}
			if (m_noMoreChunks || !addChunk(size))
			{
				m_noMoreChunks = true;
				return CHAddr::getInvalid();
			}
		}
	}

	void free(CHAddr addr)
	{
		if (!addr.internal)
		{
			cemuLog_log(LogType::Force, "Internal heap error. {:08x} {:08x}", addr.chunkIndex, addr.offset);
			return;
		}
		Block* b = static_cast<Block*>(addr.internal);
		m_numAllocatedBytes -= b->size;
		m_free.release(m_blocks, b);
	}

	virtual uint32 allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize) = 0;

private:
	// first fitting block, searching size classes upwards from size's class
	Block* findFit(uint32 size, uint32 alignment, uint32& startOut)
	{
		uint32 candidates = m_free.nonEmptyMask() & (~0u << heap_detail::sizeClass(size));
		candidates &= 0x7FFFFFFFu; // blocks of 2GB or more are not supported
		while (candidates)
		{
			const uint32 cls = (uint32)std::countr_zero(candidates);
			candidates &= candidates - 1;
			for (Block* b = m_free.first(cls); b; b = b->nextInClass)
			{
				if (heap_detail::fitsIn(b, size, alignment, startOut))
					return b;
			}
		}
		return nullptr;
	}

	bool addChunk(uint32 minimumSize)
	{
		const uint32 index = m_chunkCount++;
		const uint32 chunkSize = allocateNewChunk(index, minimumSize);
		cemu_assert_debug((chunkSize % TMinimumAlignment) == 0);
		if (chunkSize == 0)
			return false;
		cemu_assert_debug(chunkSize < 0x80000000u);
		m_free.insert(m_blocks.create(0, chunkSize, index, true));
		m_numHeapBytes += chunkSize;
		return true;
	}

	heap_detail::PooledBlocks m_blocks;
	heap_detail::FreeBlockIndex<heap_detail::PooledBlocks> m_free;
	uint32 m_chunkCount{ 0 };
	bool m_noMoreChunks{ false };

public:
	// statistics
	uint32 m_numHeapBytes{};      // sum of all chunk sizes
	uint32 m_numAllocatedBytes{}; // bytes currently handed out
};

class VGenericHeap
{
public:
	virtual void* alloc(uint32 size, uint32 alignment) = 0;
	virtual void free(void* addr) = 0;
};

// Heap over one fixed range [heapBase, heapBase + heapSize). Allocations are
// tracked by their offset. The base may be null at construction and set
// later with setHeapBase while nothing is allocated.
class VHeap : public VGenericHeap
{
	using Block = heap_detail::Block;

public:
	VHeap(void* heapBase, uint32 heapSize) : m_heapBase((uint8*)heapBase), m_heapSize(heapSize)
	{
		m_free.insert(m_blocks.create(0, heapSize, 0, true));
	}

	~VHeap()
	{
		// walk the address chain from any block and delete all of them
		Block* any = nullptr;
		for (uint32 cls = 0; cls < 32 && !any; cls++)
			any = m_free.first(cls);
		if (!any && !m_used.empty())
			any = m_used.begin()->second;
		if (!any)
			return;
		while (any->prevAdj)
			any = any->prevAdj;
		while (any)
		{
			Block* next = any->nextAdj;
			m_blocks.destroy(any);
			any = next;
		}
	}

	VHeap(const VHeap&) = delete;
	VHeap& operator=(const VHeap&) = delete;

	void setHeapBase(void* heapBase)
	{
		cemu_assert_debug(m_used.empty());
		m_heapBase = (uint8*)heapBase;
	}

	// nullptr when there is no room
	void* alloc(uint32 size, uint32 alignment = 4) override
	{
		cemu_assert_debug(m_heapBase != nullptr);
		uint32 offset;
		if (!allocOffset(size, alignment, offset))
			return nullptr;
		return m_heapBase + offset;
	}

	void free(void* addr) override
	{
		freeOffset((uint32)((uint8*)addr - m_heapBase));
	}

	bool allocOffset(uint32 size, uint32 alignment, uint32& offsetOut)
	{
		if (size == 0)
		{
			cemu_assert_suspicious();
			size = 1;
		}
		// size classes from size's own class upwards, newest block first
		for (uint32 cls = heap_detail::sizeClass(size); cls < 32; cls++)
		{
			for (Block* b = m_free.first(cls); b; b = b->nextInClass)
			{
				uint32 start;
				if (!heap_detail::fitsIn(b, size, alignment, start))
					continue;
				m_free.carve(m_blocks, b, start, size);
				m_bytesInUse += size;
				m_used.emplace(start, b);
				offsetOut = start;
				return true;
			}
		}
		return false;
	}

	void freeOffset(uint32 offset)
	{
		auto it = m_used.find(offset);
		if (it == m_used.end())
		{
			cemuLog_log(LogType::Force, "VHeap internal error");
			cemu_assert(false);
			return;
		}
		Block* b = it->second;
		m_used.erase(it);
		m_bytesInUse -= b->size;
		m_free.release(m_blocks, b);
	}

	uint32 getAllocationSizeFromAddr(void* addr)
	{
		auto it = m_used.find((uint32)((uint8*)addr - m_heapBase));
		if (it == m_used.end())
		{
			assert_dbg();
			return 0;
		}
		return it->second->size;
	}

	bool hasAllocations()
	{
		return !m_used.empty();
	}

	void getStats(uint32& heapSize, uint32& allocationSize, uint32& allocNum)
	{
		heapSize = m_heapSize;
		allocationSize = m_bytesInUse;
		allocNum = (uint32)m_used.size();
	}

private:
	heap_detail::HeapBlocks m_blocks;
	heap_detail::FreeBlockIndex<heap_detail::HeapBlocks> m_free;
	std::unordered_map<uint32, Block*> m_used;
	uint8* m_heapBase;
	const uint32 m_heapSize;
	uint32 m_bytesInUse{ 0 };
};

// Bump allocator on top of a VGenericHeap: memory is taken from the base heap
// in TChunkSize blocks (256-byte aligned) and only returned all at once by
// releaseAll(). A request must not be larger than TChunkSize.
template<uint32 TChunkSize>
class ChunkedFlatAllocator
{
public:
	void setBaseAllocator(VGenericHeap* baseHeap)
	{
		m_base = baseHeap;
	}

	void* alloc(uint32 size, uint32 alignment = 4)
	{
		cemu_assert_debug(size <= TChunkSize);
		while (true)
		{
			if (m_block)
			{
				m_used = (m_used + alignment - 1) & ~(alignment - 1);
				if (m_used + size <= TChunkSize)
				{
					void* p = m_block + m_used;
					m_used += size;
					return p;
				}
			}
			m_block = (uint8*)m_base->alloc(TChunkSize, 256);
			m_used = 0;
			m_blocks.emplace_back(m_block);
		}
	}

	void releaseAll()
	{
		for (void* p : m_blocks)
			m_base->free(p);
		m_blocks.clear();
		m_block = nullptr;
		m_used = 0;
	}

	void forEachBlock(void (*funcCb)(void* mem, uint32 size))
	{
		for (void* p : m_blocks)
			funcCb(p, TChunkSize);
	}

	uint32 getCurrentBlockOffset() const { return m_used; }
	uint8* getCurrentBlockPtr() const { return m_block; }

private:
	VGenericHeap* m_base{};
	uint8* m_block{};
	uint32 m_used{};
	std::vector<void*> m_blocks;
};
