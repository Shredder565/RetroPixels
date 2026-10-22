// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Runs enter() now and leave() when the object goes out of scope.
template<typename TEnter, typename TLeave>
class TempState
{
public:
	TempState(TEnter enter, TLeave leave) : m_leave(std::move(leave))
	{
		enter();
	}

	~TempState()
	{
		m_leave();
	}

private:
	TLeave m_leave;
};
