// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/crypto/md5.h"

#include <cmath>
#include <algorithm>
#include <cstring>

namespace
{
	// the per-step left rotations, four for each round
	constexpr int kShift[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};

	// T[i] = floor(2^32 * |sin(i + 1)|), as RFC 1321 defines the table
	struct SineTable
	{
		uint32_t t[64];
		SineTable()
		{
			for (int i = 0; i < 64; i++)
				t[i] = static_cast<uint32_t>(std::floor(std::fabs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
		}
	};
	const SineTable kSine;

	uint32_t RotateLeft(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

	uint32_t LoadLE(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

	void StoreLE(uint8_t* p, uint32_t v)
	{
		for (int i = 0; i < 4; i++)
			p[i] = static_cast<uint8_t>(v >> (8 * i));
	}
} // namespace

MD5Hash::MD5Hash() : m_state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476} {}

void MD5Hash::Compress(const uint8_t block[64])
{
	uint32_t x[16];
	for (int i = 0; i < 16; i++)
		x[i] = LoadLE(block + 4 * i);
	uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
	for (int step = 0; step < 64; step++)
	{
		const int round = step / 16;
		uint32_t f;
		int word;
		switch (round)
		{
		case 0: f = (b & c) | (~b & d); word = step; break;
		case 1: f = (b & d) | (c & ~d); word = (5 * step + 1) % 16; break;
		case 2: f = b ^ c ^ d; word = (3 * step + 5) % 16; break;
		default: f = c ^ (b | ~d); word = (7 * step) % 16; break;
		}
		const uint32_t next = b + RotateLeft(a + f + x[word] + kSine.t[step], kShift[round][step % 4]);
		a = d;
		d = c;
		c = b;
		b = next;
	}
	m_state[0] += a;
	m_state[1] += b;
	m_state[2] += c;
	m_state[3] += d;
}

void MD5Hash::Update(const void* data, size_t size)
{
	const uint8_t* p = static_cast<const uint8_t*>(data);
	size_t have = m_bytes % 64;
	m_bytes += size;
	while (size > 0)
	{
		const size_t take = std::min<size_t>(64 - have, size);
		std::memcpy(m_pending + have, p, take);
		have += take;
		p += take;
		size -= take;
		if (have == 64)
		{
			Compress(m_pending);
			have = 0;
		}
	}
}

void MD5Hash::Final(uint8_t digest[16])
{
	const uint64_t bits = m_bytes * 8;
	const uint8_t one = 0x80, zero = 0;
	Update(&one, 1);
	while (m_bytes % 64 != 56)
		Update(&zero, 1);
	uint8_t length[8];
	for (int i = 0; i < 8; i++)
		length[i] = static_cast<uint8_t>(bits >> (8 * i));
	Update(length, 8);
	for (int i = 0; i < 4; i++)
		StoreLE(digest + 4 * i, m_state[i]);
}

void MD5Hash::Of(const void* data, size_t size, uint8_t digest[16])
{
	MD5Hash h;
	h.Update(data, size);
	h.Final(digest);
}

void hmacMD5(const uint8_t* key, size_t keyLen, const uint8_t* text, size_t textLen, uint8_t digest[16])
{
	uint8_t k[64] = {};
	if (keyLen > 64)
		MD5Hash::Of(key, keyLen, k);
	else
		std::memcpy(k, key, keyLen);
	uint8_t pad[64];
	for (int i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	MD5Hash inner;
	inner.Update(pad, 64);
	inner.Update(text, textLen);
	uint8_t innerDigest[16];
	inner.Final(innerDigest);
	for (int i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x5c;
	MD5Hash outer;
	outer.Update(pad, 64);
	outer.Update(innerDigest, 16);
	outer.Final(digest);
}
