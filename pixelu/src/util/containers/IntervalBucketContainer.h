// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Spatial hash for half-open address ranges [begin, end). The address space
// is cut into slots of TAddressGranularity bytes which map onto TBucketCount
// buckets (wrapping around). A range is stored in every bucket it touches, at
// most once per bucket.
template<typename TData, typename TAddr, TAddr TAddressGranularity, int TBucketCount>
class IntervalBucketContainer
{
public:
	IntervalBucketContainer() = default;

	void addRange(TAddr rangeStart, TAddr rangeEnd, TData* data)
	{
		const Span s = spanOf(rangeStart, rangeEnd);
		for (int i = 0; i < s.count; i++)
			m_buckets[wrap(s.first + i)].push_back(Entry{ rangeStart, rangeEnd, data, s.first });
	}

	// removes the entry for data; the range must be the one it was added with
	void removeRange(TAddr rangeStart, TAddr rangeEnd, TData* data)
	{
		const Span s = spanOf(rangeStart, rangeEnd);
		int removed = 0;
		for (int i = 0; i < s.count; i++)
		{
			auto& bucket = m_buckets[wrap(s.first + i)];
			auto it = std::find_if(bucket.begin(), bucket.end(), [data](const Entry& e) { return e.data == data; });
			if (it == bucket.end())
				continue;
			assert(it->begin == rangeStart && it->end == rangeEnd);
			bucket.erase(it);
			removed++;
		}
		assert(removed == s.count);
		(void)removed;
	}

	// Calls cb(TData*) for each stored range overlapping [rangeStart, rangeEnd).
	// Buckets are visited from the first touched one onwards and entries in
	// insertion order. Beyond the first bucket, only entries whose home bucket
	// is the visited one are reported, so each range is reported once.
	template<typename TRangeCallback>
	void lookupRanges(TAddr rangeStart, TAddr rangeEnd, TRangeCallback cb)
	{
		const Span s = spanOf(rangeStart, rangeEnd);
		for (int i = 0; i < s.count; i++)
		{
			const int b = wrap(s.first + i);
			for (auto& e : m_buckets[b])
			{
				if (e.begin >= rangeEnd || e.end <= rangeStart)
					continue;
				if (i != 0 && e.homeBucket != b)
					continue;
				cb(e.data);
			}
		}
	}

private:
	struct Entry
	{
		TAddr begin;
		TAddr end;
		TData* data;
		int homeBucket;
	};

	struct Span
	{
		int first; // already wrapped into [0, TBucketCount)
		int count;
	};

	static int wrap(int index)
	{
		return index % TBucketCount;
	}

	static Span spanOf(TAddr rangeStart, TAddr rangeEnd)
	{
		assert(rangeStart < rangeEnd);
		const int firstSlot = (int)(rangeStart / TAddressGranularity);
		const int endSlot = (int)((rangeEnd + TAddressGranularity - 1) / TAddressGranularity);
		Span s;
		s.first = wrap(firstSlot);
		s.count = std::min(endSlot - firstSlot, TBucketCount);
		assert(s.count != 0);
		return s;
	}

	std::vector<Entry> m_buckets[TBucketCount];
};
