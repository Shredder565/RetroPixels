// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <mutex>
#include <memory>

// Process-wide instance that lives until exit.
template<typename T>
class SingletonClass
{
public:
	static T* getInstance()
	{
		static T s_object;
		return &s_object;
	}

protected:
	SingletonClass() = default;
};

// Shared instance that exists while someone holds a reference to it. Once the
// last shared_ptr is gone the object is destroyed and the next getInstance()
// creates a new one.
template<typename T>
class SingletonRef
{
public:
	static std::shared_ptr<T> getInstance()
	{
		std::lock_guard guard(s_guard);
		std::shared_ptr<T> live = s_weak.lock();
		if (live)
			return live;
		live = std::make_shared<T>();
		s_weak = live;
		return live;
	}

protected:
	SingletonRef() = default;

private:
	static inline std::weak_ptr<T> s_weak;
	static inline std::mutex s_guard;
};
