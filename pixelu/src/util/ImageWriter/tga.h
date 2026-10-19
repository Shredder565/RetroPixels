// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Common/FileStream.h"
#include <vector>

// Writes an uncompressed 32-bit true-color TGA (top-left origin) from
// tightly packed RGBA8 pixels. Returns false if the file can't be created.
static bool tga_write_rgba(const fs::path& path, sint32 width, sint32 height, uint8* pixelData)
{
	FileStream* file = FileStream::createFile2(path);
	if (!file)
		return false;

	uint8 header[18] = {};
	header[2] = 2; // uncompressed true-color
	header[12] = (uint8)(width & 0xFF);
	header[13] = (uint8)((width >> 8) & 0xFF);
	header[14] = (uint8)(height & 0xFF);
	header[15] = (uint8)((height >> 8) & 0xFF);
	header[16] = 32;   // bits per pixel
	header[17] = 0x20; // origin at the top
	file->writeData(header, sizeof(header));

	// TGA stores BGRA
	const size_t pixelCount = (size_t)width * (size_t)height;
	std::vector<uint8> bgra(pixelCount * 4);
	for (size_t i = 0; i < pixelCount; i++)
	{
		const uint8* src = pixelData + i * 4;
		uint8* dst = bgra.data() + i * 4;
		dst[0] = src[2];
		dst[1] = src[1];
		dst[2] = src[0];
		dst[3] = src[3];
	}
	file->writeData(bgra.data(), (sint32)bgra.size());
	delete file;
	return true;
}
