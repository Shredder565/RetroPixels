// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// std::array indexed by an enum class. The enum must end with ENUM_COUNT.
template<typename TEnum, class TValue>
class enum_array : public std::array<TValue, static_cast<size_t>(TEnum::ENUM_COUNT)>
{
	using base = std::array<TValue, static_cast<size_t>(TEnum::ENUM_COUNT)>;

public:
	TValue& operator[](TEnum index)
	{
		return base::operator[](static_cast<size_t>(index));
	}

	const TValue& operator[](TEnum index) const
	{
		return base::operator[](static_cast<size_t>(index));
	}
};
