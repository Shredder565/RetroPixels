// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <wx/string.h>
#include <wx/bitmap.h>
#include <wx/mstream.h>

namespace wxHelper
{
	// wxString (any encoding) to a filesystem path, going through UTF-8
	inline fs::path MakeFSPath(const wxString& str)
	{
		const auto utf8 = str.ToUTF8();
		return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.length()));
	}

	// filesystem path to a wxString, going through UTF-8
	inline wxString FromPath(const fs::path& path)
	{
		return wxString::FromUTF8(_pathToUtf8(path));
	}

	// Colour for every second row of a list drawn on bgColour: a bit lighter on dark backgrounds, a bit
	// darker on light ones; on almost white backgrounds a light blue-grey instead.
	inline wxColour CalculateAccentColour(const wxColour& bgColour)
	{
		const uint32 brightness = ((uint32)bgColour.Red() + bgColour.Green() + bgColour.Blue()) / 3;
		if (brightness > 250)
			return wxColour(bgColour.Red() - 13, bgColour.Green() - 6, bgColour.Blue() - 2);
		return bgColour.ChangeLightness(brightness < 128 ? 110 : 90);
	}

	// Decodes a PNG and replaces its pure black pixels with tint (icons that follow the theme's text colour)
	static wxBitmap LoadThemedBitmapFromPNG(const uint8* data, size_t size, const wxColour& tint)
	{
		wxMemoryInputStream input(data, size);
		wxImage image(input, wxBITMAP_TYPE_PNG);
		image.Replace(0, 0, 0, tint.Red(), tint.Green(), tint.Blue());
		return wxBitmap(image);
	}
};
