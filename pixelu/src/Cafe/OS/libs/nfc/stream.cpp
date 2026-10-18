// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "stream.h"

namespace nfc
{
	bool ByteReader::Take(size_t n)
	{
		if (m_failed || n > Left())
		{
			m_failed = true;
			m_pos = m_bytes.size();
			return false;
		}
		return true;
	}

	uint8 ByteReader::U8()
	{
		if (!Take(1))
			return 0;
		return m_bytes[m_pos++];
	}

	uint16 ByteReader::U16()
	{
		if (!Take(2))
			return 0;
		uint16 v = (uint16)((m_bytes[m_pos] << 8) | m_bytes[m_pos + 1]);
		m_pos += 2;
		return v;
	}

	uint32 ByteReader::U32()
	{
		if (!Take(4))
			return 0;
		uint32 v = 0;
		for (int i = 0; i < 4; i++)
			v = (v << 8) | m_bytes[m_pos + i];
		m_pos += 4;
		return v;
	}

	void ByteReader::Bytes(size_t n, std::vector<uint8>& out)
	{
		out.clear();
		if (!Take(n))
			return;
		out.assign(m_bytes.begin() + m_pos, m_bytes.begin() + m_pos + n);
		m_pos += n;
	}

	void ByteWriter::U16(uint16 v)
	{
		m_out.push_back((uint8)(v >> 8));
		m_out.push_back((uint8)v);
	}

	void ByteWriter::U32(uint32 v)
	{
		for (int shift = 24; shift >= 0; shift -= 8)
			m_out.push_back((uint8)(v >> shift));
	}
}
