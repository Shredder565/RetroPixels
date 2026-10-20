// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <string>
#include <cstring>

// Serialisation primitives of the NEX RPC protocol. Everything on the wire is
// little endian; strings carry a u16 length that counts the terminating NUL
// and byte buffers carry a u32 length.

#ifndef FFL_SIZE
#define FFL_SIZE	(0x60)
#endif

class nexPacketBuffer;

// a structure sent inside an "any data holder": name, two lengths, payload
class nexMetaType
{
public:
	virtual ~nexMetaType() = default;
	virtual const char* getMetaName() const = 0;
	virtual void writeData(nexPacketBuffer* pb) const = 0;
};

// a structure that can be read from and written to a packet
class nexType
{
public:
	virtual ~nexType() {}

	virtual const char* getMetaName()
	{
		cemu_assert_unimplemented();
		return "";
	}
	virtual void writeData(nexPacketBuffer* pb) const = 0;
	virtual void readData(nexPacketBuffer* pb) = 0;
};

// Cursor over a caller owned byte array. Writes that do not fit are dropped
// as a whole. Reads past the end yield zero and set a sticky error flag.
class nexPacketBuffer
{
public:
	nexPacketBuffer(uint8* data, sint32 size, bool isWrite)
		: m_data(data), m_size(size), m_pos(0) { (void)isWrite; } // direction is implied by use

	nexPacketBuffer() = default;

	void writeData(const uint8* data, sint32 len)
	{
		if (!HasRoom(len))
			return;
		if (len > 0)
			memcpy(m_data + m_pos, data, len);
		m_pos += len;
	}

	void writeU8(uint8 v) { Put(v); }
	void writeU16(uint16 v) { Put(v); }
	void writeU32(uint32 v) { Put(v); }
	void writeU64(uint64 v) { Put(v); }

	void writeString(const char* v)
	{
		const sint32 lenWithNul = (sint32)strlen(v) + 1;
		writeU16((uint16)lenWithNul);
		writeData((const uint8*)v, lenWithNul);
	}

	void writeBuffer(const uint8* v, sint32 len)
	{
		writeU32((uint32)len);
		writeData(v, len);
	}

	void writeCustomType(const nexMetaType& v)
	{
		writeString(v.getMetaName());
		// outer length (payload + 4) followed by the inner length (payload)
		const sint32 outerLenPos = m_pos;
		writeU32(0);
		const sint32 innerLenPos = m_pos;
		writeU32(0);
		const sint32 payloadStart = m_pos;
		v.writeData(this);
		const uint32 payloadSize = (uint32)(m_pos - payloadStart);
		PatchU32(innerLenPos, payloadSize);
		PatchU32(outerLenPos, payloadSize + 4);
	}

	uint64 readU64() { return Get<uint64>(); }
	uint32 readU32() { return Get<uint32>(); }
	uint16 readU16() { return Get<uint16>(); }
	uint8 readU8() { return Get<uint8>(); }

	sint32 readData(void* buffer, sint32 length)
	{
		if (!HasRoom(length))
		{
			m_readFailed = true;
			return 0;
		}
		if (length > 0)
			memcpy(buffer, m_data + m_pos, length);
		m_pos += length;
		return length;
	}

	// Reads a u32 length prefixed blob. At most maxLength bytes are stored but
	// the whole blob is consumed. Returns the number of bytes stored.
	sint32 readBuffer(void* buffer, sint32 maxLength)
	{
		const sint32 blobLen = (sint32)readU32();
		if (blobLen < 0 || blobLen >= 0x10000000 || !HasRoom(blobLen))
		{
			m_readFailed = true;
			return 0;
		}
		const sint32 stored = blobLen < maxLength ? blobLen : maxLength;
		if (stored > 0)
			memcpy(buffer, m_data + m_pos, stored);
		m_pos += blobLen;
		return stored;
	}

	// Reads a u16 length prefixed string into a char array of maxLength bytes,
	// always NUL terminating it. Returns the number of characters stored.
	sint32 readString(char* buffer, sint32 maxLength)
	{
		const sint32 strLen = readU16();
		if (!HasRoom(strLen))
		{
			m_readFailed = true;
			buffer[0] = '\0';
			return 0;
		}
		const sint32 stored = strLen < (maxLength - 1) ? strLen : (maxLength - 1);
		if (stored > 0)
			memcpy(buffer, m_data + m_pos, stored);
		buffer[stored] = '\0';
		m_pos += strLen;
		return stored;
	}

	// Reads a u16 length prefixed string, dropping one trailing NUL if present.
	sint32 readStdString(std::string& outputStr)
	{
		const sint32 strLen = readU16();
		if (!HasRoom(strLen))
		{
			m_readFailed = true;
			outputStr.clear();
			return 0;
		}
		sint32 keep = strLen;
		if (keep > 0 && m_data[m_pos + keep - 1] == '\0')
			keep--;
		outputStr.assign((const char*)(m_data + m_pos), keep);
		m_pos += strLen;
		return keep;
	}

	// Reads an "any data holder" whose type name has to match v's name.
	bool readPlaceholderType(nexType& v)
	{
		char typeName[128];
		readString(typeName, sizeof(typeName));
		if (m_readFailed || strcmp(typeName, v.getMetaName()) != 0)
			return false;
		const uint32 outerLen = readU32();
		const uint32 innerLen = readU32();
		if (m_readFailed || innerLen != outerLen - 4)
			return false;
		v.readData(this);
		return !m_readFailed;
	}

	bool hasReadOutOfBounds() { return m_readFailed; }
	uint8* getDataPtr() { return m_data; }
	sint32 getWriteIndex() { return m_pos; }
	sint32 getSize() { return m_size; }
	void setSize(sint32 length) { m_size = length; }

private:
	bool HasRoom(sint32 n) const
	{
		return m_pos + n <= m_size;
	}

	template<typename T>
	void Put(T v)
	{
		if (!HasRoom((sint32)sizeof(T)))
			return;
		memcpy(m_data + m_pos, &v, sizeof(T));
		m_pos += (sint32)sizeof(T);
	}

	template<typename T>
	T Get()
	{
		T v{};
		if (!HasRoom((sint32)sizeof(T)))
		{
			m_readFailed = true;
			return v;
		}
		memcpy(&v, m_data + m_pos, sizeof(T));
		m_pos += (sint32)sizeof(T);
		return v;
	}

	// a length field that did not fit is still filled as far as the buffer goes
	void PatchU32(sint32 pos, uint32 v)
	{
		const sint32 n = std::min<sint32>(4, m_size - pos);
		if (pos >= 0 && n > 0)
			memcpy(m_data + pos, &v, n);
	}

	uint8* m_data{nullptr};
	sint32 m_size{0};
	sint32 m_pos{0};
	bool m_readFailed{false};
};

// Reads a string the way the friend structures store it: through a char array
// of the given capacity, so it is cut at capacity-1 characters and at the first NUL.
inline std::string nexReadBoundedString(nexPacketBuffer* pb, sint32 capacity)
{
	std::string tmp(capacity, '\0');
	pb->readString(tmp.data(), capacity);
	tmp.resize(strlen(tmp.c_str()));
	return tmp;
}

class nexNintendoLoginData : public nexMetaType
{
public:
	nexNintendoLoginData(const char* nexToken) : m_token(nexToken) {}

	const char* getMetaName() const override
	{
		return "NintendoLoginData";
	}

	void writeData(nexPacketBuffer* pb) const override
	{
		pb->writeString(m_token.c_str());
	}

private:
	std::string m_token;
};

class nexNotificationEventGeneral : public nexType
{
public:
	nexNotificationEventGeneral() = default;

	nexNotificationEventGeneral(nexPacketBuffer* pb)
	{
		readData(pb);
	}

	const char* getMetaName() override
	{
		return "NintendoNotificationEventGeneral";
	}

	void writeData(nexPacketBuffer* pb) const override
	{
		cemu_assert_unimplemented();
	}

	void readData(nexPacketBuffer* pb) override
	{
		param1 = pb->readU32();
		param2 = pb->readU64();
		param3 = pb->readU64();
		pb->readStdString(paramStr);
	}

public:
	uint32 param1{};
	uint64 param2{};
	uint64 param3{};
	std::string paramStr;
};
