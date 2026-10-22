// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Small big-endian byte reader/writer used by the NFC tag parsers.
#pragma once

#include <span>
#include <vector>

namespace nfc
{
	// Reads big-endian values from a byte range. Reading past the end sets a
	// sticky failure flag and yields zeros, so a parser can read a whole header
	// and check Ok() once.
	class ByteReader
	{
	public:
		explicit ByteReader(std::span<const uint8> bytes) : m_bytes(bytes) {}

		uint8 U8();
		uint16 U16();
		uint32 U32();
		// copies the next n bytes into out (out is resized to n)
		void Bytes(size_t n, std::vector<uint8>& out);

		bool Ok() const { return !m_failed; }
		size_t Left() const { return m_bytes.size() - m_pos; }
		size_t Pos() const { return m_pos; }

	private:
		bool Take(size_t n);

		std::span<const uint8> m_bytes;
		size_t m_pos = 0;
		bool m_failed = false;
	};

	// Appends big-endian values to a byte vector.
	class ByteWriter
	{
	public:
		explicit ByteWriter(std::vector<uint8>& out) : m_out(out) {}

		void U8(uint8 v) { m_out.push_back(v); }
		void U16(uint16 v);
		void U32(uint32 v);
		void Bytes(std::span<const uint8> v) { m_out.insert(m_out.end(), v.begin(), v.end()); }

	private:
		std::vector<uint8>& m_out;
	};
}
