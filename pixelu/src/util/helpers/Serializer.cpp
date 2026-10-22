// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Serializer.h"

template<typename T>
T MemStreamReader::readRaw(bool bigEndian)
{
	T v{};
	if (!fits(sizeof(T)))
		return 0;
	memcpy(&v, m_data + m_cursorPos, sizeof(T));
	m_cursorPos += (sint32)sizeof(T);
	return bigEndian ? _BE(v) : _LE(v);
}

template<> uint8 MemStreamReader::readBE<uint8>() { return readRaw<uint8>(true); }
template<> uint16 MemStreamReader::readBE<uint16>() { return readRaw<uint16>(true); }
template<> uint32 MemStreamReader::readBE<uint32>() { return readRaw<uint32>(true); }
template<> uint64 MemStreamReader::readBE<uint64>() { return readRaw<uint64>(true); }

template<> uint8 MemStreamReader::readLE<uint8>() { return readRaw<uint8>(false); }
template<> uint32 MemStreamReader::readLE<uint32>() { return readRaw<uint32>(false); }
template<> uint64 MemStreamReader::readLE<uint64>() { return readRaw<uint64>(false); }

template<>
std::string MemStreamReader::readBE<std::string>()
{
	const uint32 length = readBE<uint32>();
	if (hasError())
		return {};
	constexpr uint32 kSanityLimit = 32 * 1024 * 1024;
	if (length >= kSanityLimit)
	{
		m_hasError = true;
		return {};
	}
	std::string s(length, '\0');
	readData(s.data(), length);
	return s;
}

template<typename T>
void MemStreamWriter::writeRaw(T v, bool bigEndian)
{
	const T stored = bigEndian ? _BE(v) : _LE(v);
	writeData(&stored, sizeof(T));
}

template<> void MemStreamWriter::writeBE<uint8>(const uint8& v) { m_buffer.push_back(v); }
template<> void MemStreamWriter::writeBE<uint16>(const uint16& v) { writeRaw<uint16>(v, true); }
template<> void MemStreamWriter::writeBE<uint32>(const uint32& v) { writeRaw<uint32>(v, true); }
template<> void MemStreamWriter::writeBE<uint64>(const uint64& v) { writeRaw<uint64>(v, true); }

template<> void MemStreamWriter::writeLE<uint32>(const uint32& v) { writeRaw<uint32>(v, false); }
template<> void MemStreamWriter::writeLE<uint64>(const uint64& v) { writeRaw<uint64>(v, false); }

template<>
void MemStreamWriter::writeBE<std::string>(const std::string& v)
{
	writeBE<uint32>((uint32)v.size());
	writeData(v.data(), v.size());
}
