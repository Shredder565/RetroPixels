// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <functional>

// Stores objects tagged with half-open address ranges [start, end) and finds
// those overlapping a query range. Addresses are grouped into slots of
// `granularity` bytes, hashed onto `count` buckets; each entry is listed in
// every bucket its range touches. A per-query stamp keeps an entry that sits
// in several visited buckets from being reported twice.
template<typename _OBJ, typename _ADDR, size_t count, size_t granularity>
class RangeStore
{
public:
	struct rangeEntry_t
	{
		_ADDR start;
		_ADDR end;
		_OBJ data;
		size_t lastIterationIndex;
	};

	RangeStore() = default;

	RangeStore(const RangeStore&) = delete;
	RangeStore& operator=(const RangeStore&) = delete;

	size_t getBucket(_ADDR addr)
	{
		return (size_t)(addr / granularity) % count;
	}

	// first bucket and number of slots spanned (not capped at count)
	void getBucketRange(_ADDR addrStart, _ADDR addrEnd, size_t& bucketFirst, size_t& bucketCount)
	{
		const _ADDR last = std::max(addrStart, (_ADDR)(addrEnd - 1));
		bucketFirst = getBucket(addrStart);
		bucketCount = (size_t)(last / granularity) - (size_t)(addrStart / granularity) + 1;
	}

	// end is start + size; returns a handle for deleteRange
	void* storeRange(_OBJ data, _ADDR start, _ADDR end)
	{
		rangeEntry_t* e = new rangeEntry_t{ start, end, data, m_stamp };
		forEachBucket(start, end, [e](std::vector<rangeEntry_t*>& bucket) { bucket.push_back(e); });
		return e;
	}

	void deleteRange(void* rangePtr)
	{
		rangeEntry_t* e = static_cast<rangeEntry_t*>(rangePtr);
		forEachBucket(e->start, e->end, [e](std::vector<rangeEntry_t*>& bucket) {
			bucket.erase(std::remove(bucket.begin(), bucket.end(), e), bucket.end());
		});
		delete e;
	}

	// calls f for every overlapping entry, in bucket order then insertion order
	void findRanges(_ADDR start, _ADDR end, std::function<void(_ADDR start, _ADDR end, _OBJ data)> f)
	{
		m_stamp++;
		forEachBucket(start, end, [&](std::vector<rangeEntry_t*>& bucket) {
			for (rangeEntry_t* e : bucket)
			{
				if (overlaps(e, start, end) && e->lastIterationIndex != m_stamp)
				{
					e->lastIterationIndex = m_stamp;
					f(e->start, e->end, e->data);
				}
			}
		});
	}

	// the first entry findRanges would report, if any
	bool findFirstRange(_ADDR start, _ADDR end, _ADDR& rStart, _ADDR& rEnd, _OBJ& rData)
	{
		m_stamp++;
		size_t first, n;
		getBucketRange(start, end, first, n);
		n = std::min(n, count);
		for (size_t i = 0; i < n; i++)
		{
			for (rangeEntry_t* e : m_buckets[(first + i) % count])
			{
				if (overlaps(e, start, end) && e->lastIterationIndex != m_stamp)
				{
					e->lastIterationIndex = m_stamp;
					rStart = e->start;
					rEnd = e->end;
					rData = e->data;
					return true;
				}
			}
		}
		return false;
	}

	void clear()
	{
		for (auto& bucket : m_buckets)
		{
			while (!bucket.empty())
				deleteRange(bucket.front());
		}
	}

private:
	static bool overlaps(const rangeEntry_t* e, _ADDR start, _ADDR end)
	{
		return start < e->end && end > e->start;
	}

	template<typename TFunc>
	void forEachBucket(_ADDR start, _ADDR end, TFunc fn)
	{
		size_t first, n;
		getBucketRange(start, end, first, n);
		n = std::min(n, count);
		for (size_t i = 0; i < n; i++)
			fn(m_buckets[(first + i) % count]);
	}

	std::array<std::vector<rangeEntry_t*>, count> m_buckets;
	size_t m_stamp{ 0 };
};
