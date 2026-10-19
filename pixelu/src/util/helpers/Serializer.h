// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Bounds-checked reader over a byte buffer. A read past the end sets the
// error flag, moves the cursor to the end and returns zero/empty values.
class MemStreamReader
{
public:
	MemStreamReader(const uint8* data, sint32 size) : m_data(data), m_size(size), m_cursorPos(0) {}

	// supported: uint8, uint16, uint32, uint64, std::string (uint32 length + bytes)
	template<typename T> T readBE();
	// supported: uint8, uint32, uint64
	template<typename T> T readLE();

	// uint32 element count (big endian) followed by the raw elements
	template<typename T>
	std::vector<T> readPODVector()
	{
		const uint32 count = readBE<uint32>();
		if (hasError())
			return {};
		std::vector<T> result(count);
		readData(result.data(), result.size() * sizeof(T));
		return result;
	}

	// Next line up to '\n' or the end of the data, without the newline and
	// without '\r' characters at either end. Sets the error flag (and returns
	// an empty view) when already at the end.
	std::string_view readLine()
	{
		if (m_cursorPos >= m_size)
		{
			m_hasError = true;
			return std::string_view(nullptr, 0);
		}
		const char* lineBegin = (const char*)(m_data + m_cursorPos);
		const char* lineEnd = nullptr;
		while (m_cursorPos < m_size)
		{
			const uint8 c = m_data[m_cursorPos++];
			if (c == '\n')
			{
				lineEnd = (const char*)(m_data + m_cursorPos - 1);
				break;
			}
		}
		if (!lineEnd)
			lineEnd = (const char*)(m_data + m_cursorPos);
		while (lineBegin < lineEnd && *lineBegin == '\r')
			lineBegin++;
		while (lineEnd > lineBegin && lineEnd[-1] == '\r')
			lineEnd--;
		return std::string_view(lineBegin, (size_t)(lineEnd - lineBegin));
	}

	bool readData(void* ptr, size_t size)
	{
		if (!fits(size))
			return false;
		if (size != 0)
			memcpy(ptr, m_data + m_cursorPos, size);
		m_cursorPos += (sint32)size;
		return true;
	}

	std::span<uint8> readDataNoCopy(size_t size)
	{
		if (!fits(size))
			return {};
		std::span<uint8> view((uint8*)m_data + m_cursorPos, size);
		m_cursorPos += (sint32)size;
		return view;
	}

	// true if any read so far went out of bounds
	bool hasError() const
	{
		return m_hasError;
	}

	bool isEndOfStream() const
	{
		return m_cursorPos == m_size;
	}

private:
	template<typename T> T readRaw(bool bigEndian);

	// checks that size more bytes are available, flags an error otherwise
	bool fits(size_t size)
	{
		if ((size_t)m_cursorPos + size > (size_t)m_size)
		{
			m_cursorPos = m_size;
			m_hasError = true;
			return false;
		}
		return true;
	}

	const uint8* m_data;
	sint32 m_size;
	sint32 m_cursorPos;
	bool m_hasError{ false };
};

// Appends binary data to a growing byte vector.
class MemStreamWriter
{
public:
	MemStreamWriter(size_t reservedSize)
	{
		m_buffer.reserve(reservedSize > 0 ? reservedSize : 128);
	}

	void writeData(const void* ptr, size_t size)
	{
		const size_t start = m_buffer.size();
		m_buffer.resize(start + size);
		if (size)
			memcpy(m_buffer.data() + start, ptr, size);
	}

	// supported: uint8, uint16, uint32, uint64, std::string
	template<typename T> void writeBE(const T& v);
	// supported: uint32, uint64
	template<typename T> void writeLE(const T& v);

	template<typename T>
	void writePODVector(const std::vector<T>& v)
	{
		writeBE<uint32>((uint32)v.size());
		writeData(v.data(), v.size() * sizeof(T));
	}

	// moves the written bytes into data and leaves the writer empty
	void getResultAndReset(std::vector<uint8>& data)
	{
		std::swap(m_buffer, data);
		m_buffer.clear();
	}

	std::span<uint8> getResult()
	{
		return std::span<uint8>(m_buffer.data(), m_buffer.size());
	}

private:
	template<typename T> void writeRaw(T v, bool bigEndian);

	std::vector<uint8> m_buffer;
};

template<> uint8 MemStreamReader::readBE<uint8>();
template<> uint16 MemStreamReader::readBE<uint16>();
template<> uint32 MemStreamReader::readBE<uint32>();
template<> uint64 MemStreamReader::readBE<uint64>();
template<> std::string MemStreamReader::readBE<std::string>();
template<> uint8 MemStreamReader::readLE<uint8>();
template<> uint32 MemStreamReader::readLE<uint32>();
template<> uint64 MemStreamReader::readLE<uint64>();

template<> void MemStreamWriter::writeBE<uint8>(const uint8& v);
template<> void MemStreamWriter::writeBE<uint16>(const uint16& v);
template<> void MemStreamWriter::writeBE<uint32>(const uint32& v);
template<> void MemStreamWriter::writeBE<uint64>(const uint64& v);
template<> void MemStreamWriter::writeBE<std::string>(const std::string& v);
template<> void MemStreamWriter::writeLE<uint32>(const uint32& v);
template<> void MemStreamWriter::writeLE<uint64>(const uint64& v);

// Base for objects that convert themselves to and from a byte vector.
class SerializerHelper
{
public:
	bool serialize(std::vector<uint8>& data)
	{
		MemStreamWriter writer(0);
		if (!serializeImpl(writer))
			return false;
		writer.getResultAndReset(data);
		return true;
	}

	bool deserialize(std::vector<uint8>& data)
	{
		MemStreamReader reader(data.data(), (sint32)data.size());
		return deserializeImpl(reader);
	}

protected:
	virtual bool serializeImpl(MemStreamWriter& streamWriter) = 0;
	virtual bool deserializeImpl(MemStreamReader& streamReader) = 0;
};
