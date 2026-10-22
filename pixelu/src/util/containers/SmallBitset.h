// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Up to 32 flags packed into one uint32. Indices are only checked in debug builds.
template<size_t N>
class SmallBitset
{
	static_assert(N <= 32, "SmallBitset holds at most 32 bits");

public:
	SmallBitset() = default;

	bool test(size_t index) const
	{
		cemu_assert_debug(index < N);
		return (m_mask & bit(index)) != 0;
	}

	void set(size_t index)
	{
		cemu_assert_debug(index < N);
		m_mask |= bit(index);
	}

	void set(size_t index, bool val)
	{
		cemu_assert_debug(index < N);
		if (val)
			m_mask |= bit(index);
		else
			m_mask &= ~bit(index);
	}

private:
	static uint32 bit(size_t index) { return 1u << (uint32)index; }

	uint32 m_mask{ 0 };
};
