// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Sparse three-level radix table indexed by the low (TBitsX + TBitsY +
// TBitsZ) bits of a 32-bit key. Unused branches all point at one shared
// empty leaf / one shared empty middle table, so lookups need no null checks
// and untouched keys read back as T{}.
template<int TBitsX, int TBitsY, int TBitsZ, typename T>
class LookupTableL3
{
	static constexpr uint32 kSizeX = 1u << TBitsX;
	static constexpr uint32 kSizeY = 1u << TBitsY;
	static constexpr uint32 kSizeZ = 1u << TBitsZ;

	struct Leaf
	{
		T values[kSizeZ]{};
	};

	struct Middle
	{
		Leaf* leaves[kSizeY];
	};

public:
	LookupTableL3()
	{
		m_emptyLeaf = new Leaf();
		m_emptyMiddle = newMiddle();
		for (uint32 i = 0; i < kSizeX; i++)
			m_top[i] = m_emptyMiddle;
	}

	~LookupTableL3()
	{
		for (uint32 x = 0; x < kSizeX; x++)
		{
			Middle* m = m_top[x];
			if (m == m_emptyMiddle)
				continue;
			for (uint32 y = 0; y < kSizeY; y++)
			{
				if (m->leaves[y] != m_emptyLeaf)
					delete m->leaves[y];
			}
			delete m;
		}
		delete m_emptyMiddle;
		delete m_emptyLeaf;
	}

	LookupTableL3(const LookupTableL3&) = delete;
	LookupTableL3& operator=(const LookupTableL3&) = delete;

	// value stored for key (only the low bits are used), T{} if none
	T lookup(uint32 key)
	{
		return m_top[indexX(key)]->leaves[indexY(key)]->values[indexZ(key)];
	}

	void store(uint32 key, const T& value)
	{
		Middle*& middle = m_top[indexX(key)];
		if (middle == m_emptyMiddle)
			middle = newMiddle();
		Leaf*& leaf = middle->leaves[indexY(key)];
		if (leaf == m_emptyLeaf)
			leaf = new Leaf();
		leaf->values[indexZ(key)] = value;
	}

private:
	static uint32 indexX(uint32 key) { return (key >> (TBitsY + TBitsZ)) & (kSizeX - 1); }
	static uint32 indexY(uint32 key) { return (key >> TBitsZ) & (kSizeY - 1); }
	static uint32 indexZ(uint32 key) { return key & (kSizeZ - 1); }

	Middle* newMiddle()
	{
		Middle* m = new Middle();
		for (uint32 i = 0; i < kSizeY; i++)
			m->leaves[i] = m_emptyLeaf;
		return m;
	}

	Leaf* m_emptyLeaf{};
	Middle* m_emptyMiddle{};
	Middle* m_top[kSizeX];
};
