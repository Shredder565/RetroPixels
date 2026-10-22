// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"

#include "Cafe/OS/libs/coreinit/coreinit_OSScreen_font.h"
#include "Cafe/OS/libs/coreinit/coreinit_OSScreen.h"

// Each screen is double buffered: the title hands over one buffer holding both images back to back
// and draws into the one that is not shown. The GPU side (LatteGPUState.osScreen) shows the image
// selected by the flip count. Pixels are 32 bit RGBA, stored big endian.
namespace coreinit
{
	namespace
	{
		struct ScreenGeometry
		{
			sint32 width;
			sint32 height;
			sint32 pitch; // in pixels
		};

		constexpr ScreenGeometry kScreens[2] = {
			{ 1280, 720, 1280 }, // TV
			{ 896, 480, 896 },	 // gamepad (not verified on hardware)
		};

		// the image currently drawn to, per screen
		uint8* s_drawTarget[2] = {};

		uint32 ImageBytes(uint32 screen)
		{
			return kScreens[screen].pitch * kScreens[screen].height * 4;
		}

		void SelectDrawTarget(uint32 screen)
		{
			auto& state = LatteGPUState.osScreen.screen[screen];
			uint32 image = (state.flipRequestCount & 1) != 0 ? ImageBytes(screen) : 0;
			s_drawTarget[screen] = memory_getPointerFromPhysicalOffset(state.physPtr + image);
		}

		uint32* PixelAt(uint32 screen, sint32 x, sint32 y)
		{
			return (uint32*)(s_drawTarget[screen] + (x + y * kScreens[screen].pitch) * 4);
		}

		// The font covers the printable ASCII range starting at '!' (space has no glyph). Each glyph
		// is 25 rows of 14 pixels, two bytes per row with the leftmost pixel in the top bit. Text
		// is laid out on a grid of 16 x 24 pixel cells.
		constexpr char kFirstGlyph = '!';
		constexpr char kLastGlyph = '~';
		constexpr sint32 kGlyphRows = 25;
		constexpr sint32 kGlyphColumns = 14;
		constexpr sint32 kGlyphBytes = kGlyphRows * 2;
		constexpr sint32 kCellWidth = 16;
		constexpr sint32 kCellHeight = 24;

		void DrawGlyph(uint32 screen, sint32 px, sint32 py, sint32 glyphIndex)
		{
			const uint8* glyph = osscreenBitmapFont + glyphIndex * kGlyphBytes;
			for (sint32 row = 0; row < kGlyphRows; row++)
			{
				uint32 bits = (glyph[row * 2] << 8) | glyph[row * 2 + 1];
				for (sint32 col = 0; col < kGlyphColumns; col++)
				{
					if ((bits & (0x8000 >> col)) != 0)
						*PixelAt(screen, px + col, py + row) = 0xFFFFFFFF;
				}
			}
		}
	}

	void OSScreenInit()
	{
	}

	uint32 OSScreenGetBufferSizeEx(uint32 screenIndex)
	{
		cemu_assert(screenIndex < 2);
		return ImageBytes(screenIndex) * 2; // two images
	}

	void OSScreenSetBufferEx(uint32 screenIndex, uint32 buffer)
	{
		cemu_assert(screenIndex < 2);
		LatteGPUState.osScreen.screen[screenIndex].physPtr = buffer;
		SelectDrawTarget(screenIndex);
	}

	void OSScreenEnableEx(uint32 screenIndex, uint32 isEnabled)
	{
		cemu_assert(screenIndex < 2);
		LatteGPUState.osScreen.screen[screenIndex].isEnabled = isEnabled != 0;
	}

	void OSScreenClearBufferEx(uint32 screenIndex, uint32 color)
	{
		cemu_assert(screenIndex < 2);
		if (!s_drawTarget[screenIndex])
			return;
		uint32* pixels = (uint32*)s_drawTarget[screenIndex];
		std::fill_n(pixels, kScreens[screenIndex].pitch * kScreens[screenIndex].height, _swapEndianU32(color));
	}

	void OSScreenFlipBuffersEx(uint32 screenIndex)
	{
		cemu_assert(screenIndex < 2);
		LatteGPUState.osScreen.screen[screenIndex].flipRequestCount++;
		SelectDrawTarget(screenIndex);
	}

	void OSScreenPutPixelEx(uint32 screenIndex, sint32 x, sint32 y, uint32 color)
	{
		if (screenIndex >= 2)
			return;
		const ScreenGeometry& geometry = kScreens[screenIndex];
		if (x < 0 || x >= geometry.width || y < 0 || y >= geometry.height)
			return;
		*PixelAt(screenIndex, x, y) = _swapEndianU32(color);
	}

	// x and y are in text cells; '\n' starts a new line at the original column. Text is not clipped.
	void OSScreenPutFontEx(uint32 screenIndex, sint32 x, sint32 y, const char* str)
	{
		if (y < 0)
			return;
		sint32 px = x * kCellWidth;
		sint32 py = y * kCellHeight;
		const sint32 lineStart = px;
		for (; *str; str++)
		{
			char c = *str;
			if (c == '\n')
			{
				px = lineStart;
				py += kCellHeight;
				continue;
			}
			if (c >= kFirstGlyph && c <= kLastGlyph)
				DrawGlyph(screenIndex, px, py, c - kFirstGlyph);
			px += kCellWidth;
		}
	}

	void InitializeOSScreen()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::Placeholder)
		EXPORT_FN(OSScreenInit); EXPORT_FN(OSScreenGetBufferSizeEx); EXPORT_FN(OSScreenSetBufferEx);
		EXPORT_FN(OSScreenEnableEx); EXPORT_FN(OSScreenClearBufferEx); EXPORT_FN(OSScreenFlipBuffersEx);
		EXPORT_FN(OSScreenPutPixelEx); EXPORT_FN(OSScreenPutFontEx);
#undef EXPORT_FN
	}
}
