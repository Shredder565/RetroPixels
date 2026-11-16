// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <cstddef>
#include <cstdint>

// MD5 (RFC 1321) and HMAC-MD5 (RFC 2104), written from the RFCs.
class MD5Hash
{
  public:
	MD5Hash();
	void Update(const void* data, size_t size);
	void Final(uint8_t digest[16]);

	static void Of(const void* data, size_t size, uint8_t digest[16]);

  private:
	void Compress(const uint8_t block[64]);

	uint32_t m_state[4];
	uint64_t m_bytes = 0;
	uint8_t m_pending[64];
};

// hmacMD5 puts HMAC-MD5(key, text) in digest.
void hmacMD5(const uint8_t* key, size_t keyLen, const uint8_t* text, size_t textLen, uint8_t digest[16]);
