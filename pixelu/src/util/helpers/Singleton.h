// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Base for classes with exactly one, lazily created instance.
// Derived classes befriend Singleton<Derived> when their constructor is private.
template<typename TDerived>
class Singleton
{
protected:
	Singleton() = default;
	Singleton(const Singleton&) = delete;
	Singleton(Singleton&&) noexcept = delete;

public:
	static TDerived& instance() noexcept
	{
		static TDerived s_only;
		return s_only;
	}
};
