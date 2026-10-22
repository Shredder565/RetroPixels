// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// CRC-32 as used by zlib/PNG (reflected polynomial 0xEDB88320, initial value
// and final XOR 0xFFFFFFFF). Longer inputs use eight tables so that eight
// bytes are folded in per step; the tables are generated at compile time.

#include "crc32.h"

namespace
{
	constexpr uint32 kPolynomial = 0xEDB88320u;

	struct CrcTables
	{
		uint32 t[8][256]{};
	};

	constexpr CrcTables buildTables()
	{
		CrcTables tables{};
		for (uint32 n = 0; n < 256; n++)
		{
			uint32 v = n;
			for (int bit = 0; bit < 8; bit++)
				v = (v & 1) ? (v >> 1) ^ kPolynomial : (v >> 1);
			tables.t[0][n] = v;
		}
		// t[k][n] is the CRC of byte n followed by k zero bytes
		for (int k = 1; k < 8; k++)
		{
			for (uint32 n = 0; n < 256; n++)
			{
				const uint32 prev = tables.t[k - 1][n];
				tables.t[k][n] = (prev >> 8) ^ tables.t[0][prev & 0xFF];
			}
		}
		return tables;
	}

	constexpr CrcTables kCrc = buildTables();
	static_assert(kCrc.t[0][1] == 0x77073096u && kCrc.t[0][255] == 0x2D02EF8Du);

	inline uint32 foldByte(uint32 crc, uint8 b)
	{
		return (crc >> 8) ^ kCrc.t[0][(crc ^ b) & 0xFF];
	}
}

uint32 crc32_calc(uint32 c, const void* data, size_t length)
{
	const uint8* p = (const uint8*)data;
	uint32 crc = ~c;
	// eight bytes at a time; bytes are combined explicitly so this works on any host byte order
	while (length >= 8)
	{
		const uint32 lo = crc ^ ((uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24));
		crc = kCrc.t[7][lo & 0xFF] ^ kCrc.t[6][(lo >> 8) & 0xFF] ^ kCrc.t[5][(lo >> 16) & 0xFF] ^ kCrc.t[4][lo >> 24] ^
			kCrc.t[3][p[4]] ^ kCrc.t[2][p[5]] ^ kCrc.t[1][p[6]] ^ kCrc.t[0][p[7]];
		p += 8;
		length -= 8;
	}
	while (length--)
		crc = foldByte(crc, *p++);
	return ~crc;
}

// self-test, run from UnitTests() in debug builds
void CRCTest()
{
	uint8 bytes[89];
	for (uint32 i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8)i;
	cemu_assert(crc32_calc(0, bytes, sizeof(bytes)) == 0x3FC61683u);
	cemu_assert(crc32_calc(0, "123456789", 9) == 0xCBF43926u);
	// splitting the input must not change the result
	cemu_assert(crc32_calc(crc32_calc(0, bytes, 37), bytes + 37, sizeof(bytes) - 37) == 0x3FC61683u);
}
