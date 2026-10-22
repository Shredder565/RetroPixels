// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "util/helpers/Singleton.h"

#include <boost/signals2.hpp>
#include <boost/bind/placeholders.hpp>

enum class Events : int32_t
{
	ControllerChanged,
};

using ControllerChangedFunc = void(void);

// Process-wide notifications. Listeners connect a member function plus its
// object; signal<E>() calls every connected listener of E.
class EventService : public Singleton<EventService>
{
	friend class Singleton<EventService>;
	EventService() = default;

public:
	template<Events event, typename TFunc, typename TClass>
	boost::signals2::connection connect(TFunc function, TClass thisptr)
	{
		static_assert(event == Events::ControllerChanged, "unknown event");
		return m_controllerChanged.connect([function, thisptr]() { std::invoke(function, thisptr); });
	}

	template<Events event>
	void disconnect(const boost::signals2::connection& slot)
	{
		static_assert(event == Events::ControllerChanged, "unknown event");
		slot.disconnect();
	}

	template<Events event, typename... TArgs>
	void signal(TArgs&&... args)
	{
		static_assert(event == Events::ControllerChanged, "unknown event");
		try
		{
			m_controllerChanged(std::forward<TArgs>(args)...);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "error when signaling {}: {}", event, ex.what());
		}
	}

private:
	boost::signals2::signal<ControllerChangedFunc> m_controllerChanged;
};
