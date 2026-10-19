// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// The state a host controller reports at one point in time: its sticks and triggers
// in [-1, 1] and the ids of the buttons that are held. Button ids are the controller
// API's own numbers (see Buttons2 in Controller.h); stick directions also show up as
// button ids once they pass ControllerState::kAxisThreshold, so they can be mapped
// like buttons.

#include <glm/vec2.hpp>
#include "util/helpers/fspinlock.h"

enum class PositionVisibility {
	NONE = 0,
	FULL = 1,
	PARTIAL = 2
};

// Held buttons, safe to read from one thread while another updates them. Only a
// handful of buttons are ever held together, so they live in a short list, in the
// order they were pressed.
struct ControllerButtonState
{
	ControllerButtonState() = default;

	ControllerButtonState(const ControllerButtonState& other)
	{
		std::lock_guard guard(other.m_lock);
		m_held = other.m_held;
	}

	ControllerButtonState(ControllerButtonState&& other)
	{
		std::lock_guard guard(other.m_lock);
		m_held = std::move(other.m_held);
	}

	ControllerButtonState& operator=(const ControllerButtonState& other)
	{
		if (this != &other)
		{
			std::scoped_lock guard(m_lock, other.m_lock);
			m_held = other.m_held;
		}
		return *this;
	}

	ControllerButtonState& operator=(ControllerButtonState&& other)
	{
		if (this != &other)
		{
			std::scoped_lock guard(m_lock, other.m_lock);
			m_held = std::move(other.m_held);
		}
		return *this;
	}

	void SetButtonState(uint32 buttonId, bool isPressed)
	{
		std::lock_guard guard(m_lock);
		const auto it = std::find(m_held.begin(), m_held.end(), buttonId);
		if (isPressed)
		{
			if (it == m_held.end())
				m_held.push_back(buttonId);
		}
		else if (it != m_held.end())
		{
			m_held.erase(it);
		}
	}

	// marks every listed button as held, under one lock
	void SetPressedButtons(std::span<uint32> buttonList)
	{
		std::lock_guard guard(m_lock);
		for (const uint32 id : buttonList)
		{
			if (!contains(m_held, id))
				m_held.push_back(id);
		}
	}

	bool GetButtonState(uint32 buttonId) const
	{
		std::lock_guard guard(m_lock);
		return contains(m_held, buttonId);
	}

	// releases every button that is held in `buttonsToUnset`
	void UnsetButtons(const ControllerButtonState& buttonsToUnset)
	{
		if (this == &buttonsToUnset)
		{
			std::lock_guard guard(m_lock);
			m_held.clear();
			return;
		}
		std::scoped_lock guard(m_lock, buttonsToUnset.m_lock);
		const auto& drop = buttonsToUnset.m_held;
		std::erase_if(m_held, [&drop](uint32 id) { return contains(drop, id); });
	}

	bool IsIdle() const
	{
		std::lock_guard guard(m_lock);
		return m_held.empty();
	}

	std::vector<uint32> GetButtonList() const
	{
		std::lock_guard guard(m_lock);
		return m_held;
	}

	// same set of held buttons, in any order
	bool operator==(const ControllerButtonState& other) const
	{
		if (this == &other)
			return true;
		std::scoped_lock guard(m_lock, other.m_lock);
		if (m_held.size() != other.m_held.size())
			return false;
		return std::all_of(m_held.begin(), m_held.end(), [&other](uint32 id) { return contains(other.m_held, id); });
	}

private:
	static bool contains(const std::vector<uint32>& list, uint32 id)
	{
		return std::find(list.begin(), list.end(), id) != list.end();
	}

	std::vector<uint32> m_held;
	mutable FSpinlock m_lock;
};

struct ControllerState
{
	// how far an axis has to move before its direction counts as a pressed button
	constexpr static float kAxisThreshold = 0.1f;

	// A moved stick never reads exactly 0 on its other axis on real hardware, and some
	// games test "x != 0 && y != 0" to see whether the stick is tilted at all. Axes that
	// end up at 0 while the other one is moved get this tiny value instead.
	constexpr static float kMinAxisValue = 0.0000001f;

	// [-1, 1]
	glm::vec2 axis{ };
	glm::vec2 rotation{ };
	glm::vec2 trigger{ };

	ControllerButtonState buttons{};

	uint64 last_state = 0;

	// two states are equal when the same buttons are held (the axes are not compared)
	bool operator==(const ControllerState& other) const;
	bool operator!=(const ControllerState& other) const
	{
		return !(*this == other);
	}
};
