// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Hardware constants of the Wii U main processor (IBM Espresso, three
// PowerPC 750CL derived cores).
namespace Espresso
{
	constexpr inline int CORE_COUNT = 3;

	// core frequency in Hz
	constexpr inline uint64 CORE_CLOCK = 1243125000;
	// the bus runs at one fifth of the core clock
	constexpr inline uint64 BUS_CLOCK = 248625000;
	// time base and decrementer tick once every four bus cycles
	constexpr inline uint64 TIMER_CLOCK = BUS_CLOCK / 4;

	// Cafe OS maps memory in 128KiB pages
	constexpr inline uint32 MEM_PAGE_SIZE = 0x20000;
};
