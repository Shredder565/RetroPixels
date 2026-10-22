// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <array>

// Unordered list with a fixed capacity and no heap allocation. Removing an
// element moves the last element into its place.
template<typename T, uint32 maxElements, bool checkMaxSize = true>
class FixedSizeList
{
public:
	std::array<T, maxElements> m_elementArray;
	int count = 0;

	// ignored when full (if checkMaxSize)
	void add(T n)
	{
		if (checkMaxSize && count >= (int)maxElements)
			return;
		m_elementArray[count++] = n;
	}

	void addUnique(T n)
	{
		if (checkMaxSize && count >= (int)maxElements)
			return;
		if (find(n) >= 0)
			return;
		m_elementArray[count++] = n;
	}

	void remove(T n)
	{
		containsAndRemove(n);
	}

	bool containsAndRemove(T n)
	{
		const sint32 index = find(n);
		if (index < 0)
			return false;
		m_elementArray[index] = m_elementArray[count - 1];
		count--;
		return true;
	}

	// index of the first element equal to n, or -1
	sint32 find(T n)
	{
		for (int i = 0; i < count; i++)
		{
			if (m_elementArray[i] == n)
				return i;
		}
		return -1;
	}
};
