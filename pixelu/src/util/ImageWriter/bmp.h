// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Common/FileStream.h"
#include <vector>

// Minimal BMP writers for debugging dumps. The source rows are bytesPerRow
// apart and are converted to a 24-bit bottom-up BMP.

namespace bmp_detail
{
	inline void putLE(std::vector<uint8>& out, uint32 v, int bytes)
	{
		for (int i = 0; i < bytes; i++)
			out.push_back((uint8)(v >> (8 * i)));
	}

	// rgb: width * height * 3 bytes, top row first
	inline bool write24(const wchar_t* path, sint32 width, sint32 height, const std::vector<uint8>& rgb)
	{
		FileStream* file = FileStream::createFile(path);
		if (!file)
			return false;
		const uint32 rowBytes = (uint32)width * 3;
		const uint32 stride = (rowBytes + 3) & ~3u;
		const uint32 headerSize = 14 + 40;
		std::vector<uint8> out;
		out.reserve(headerSize + stride * (uint32)height);
		// file header
		out.push_back('B');
		out.push_back('M');
		putLE(out, headerSize + stride * (uint32)height, 4);
		putLE(out, 0, 4);
		putLE(out, headerSize, 4);
		// info header
		putLE(out, 40, 4);
		putLE(out, (uint32)width, 4);
		putLE(out, (uint32)height, 4);
		putLE(out, 1, 2);  // planes
		putLE(out, 24, 2); // bits per pixel
		for (int i = 0; i < 6; i++)
			putLE(out, 0, 4); // no compression, default sizes and palette
		for (sint32 y = height - 1; y >= 0; y--)
		{
			const uint8* row = rgb.data() + (size_t)y * rowBytes;
			out.insert(out.end(), row, row + rowBytes);
			out.resize(out.size() + (stride - rowBytes), 0);
		}
		file->writeData(out.data(), (sint32)out.size());
		delete file;
		return true;
	}

	// expands every source pixel of bytesPerPixel into three output bytes
	template<typename TConvert>
	inline bool store(const wchar_t* path, sint32 width, sint32 height, sint32 bytesPerRow, const void* pixelData, sint32 bytesPerPixel, TConvert convert)
	{
		std::vector<uint8> rgb((size_t)width * (size_t)height * 3);
		const uint8* src = static_cast<const uint8*>(pixelData);
		for (sint32 y = 0; y < height; y++)
		{
			for (sint32 x = 0; x < width; x++)
				convert(src + (size_t)y * bytesPerRow + (size_t)x * bytesPerPixel, rgb.data() + ((size_t)y * width + x) * 3);
		}
		return write24(path, width, height, rgb);
	}
}

// 8-bit gray
static bool bmp_store8BitAs24(wchar_t* path, sint32 width, sint32 height, sint32 bytesPerRow, void* pixelData)
{
	return bmp_detail::store(path, width, height, bytesPerRow, pixelData, 1, [](const uint8* s, uint8* d) { d[0] = d[1] = d[2] = s[0]; });
}

// two 8-bit channels, the third output channel is zero
static bool bmp_store16BitAs24(wchar_t* path, sint32 width, sint32 height, sint32 bytesPerRow, void* pixelData)
{
	return bmp_detail::store(path, width, height, bytesPerRow, pixelData, 2, [](const uint8* s, uint8* d) { d[0] = s[0]; d[1] = s[1]; d[2] = 0; });
}

// three 8-bit channels, copied as they are
static bool bmp_store24BitAs24(wchar_t* path, sint32 width, sint32 height, sint32 bytesPerRow, void* pixelData)
{
	return bmp_detail::store(path, width, height, bytesPerRow, pixelData, 3, [](const uint8* s, uint8* d) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; });
}
