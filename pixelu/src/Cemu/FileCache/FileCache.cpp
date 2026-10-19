// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "FileCache.h"
#include "util/helpers/helpers.h"

#include <condition_variable>
#include <deque>
#include "zlib.h"
#include "Common/FileStream.h"

// On-disk layout (all values little endian):
//   0x00 u32 magic, 0x04 u32 extra version (caller-defined cache version),
//   0x08 u64 start of the data area, 0x10 u64 file table offset (relative to
//   the data area), 0x18 u32 file table size in bytes; the header area is
//   0x80 bytes. The data area holds the stored files; the file table is an
//   array of 0x20-byte records and is stored there as a file under a reserved
//   name. A record with both names 0 is unused.
//   Compressed files start with their uncompressed size (u32 big endian)
//   followed by a zlib stream.
namespace
{
	constexpr uint32 kMagicLegacy = 0x8371b694;     // 32-bit offsets, no longer readable
	constexpr uint32 kMagicLarge = 0x8371b695;      // 64-bit offsets, no compression flag
	constexpr uint32 kMagicCompressed = 0x8371b696; // current format, written for all new caches
	constexpr uint64 kHeaderArea = 0x80;
	constexpr sint32 kInitialSlots = 32;
	constexpr sint32 kSlotsPerGrowth = 64;
	constexpr uint64 kTableName1 = 0xEFEFEFEFEFEFEFEFULL;
	constexpr uint64 kTableName2 = 0xFEFEFEFEFEFEFEFEULL;
	constexpr uint8 kFlagZlib = 0x01;
	constexpr int kZlibLevel = 4;

	bool IsUnused(uint64 name1, uint64 name2)
	{
		return name1 == 0 && name2 == 0;
	}

	bool IsTableName(uint64 name1, uint64 name2)
	{
		return name1 == kTableName1 && name2 == kTableName2;
	}

	// returns false (and leaves 'out' empty) if zlib fails
	bool Deflate(const uint8* data, uint32 size, std::vector<uint8>& out)
	{
		uLongf packedLen = compressBound(size);
		out.resize(4 + packedLen);
		if (compress2(out.data() + 4, &packedLen, data, size, kZlibLevel) != Z_OK)
		{
			out.clear();
			return false;
		}
		out[0] = (uint8)(size >> 24);
		out[1] = (uint8)(size >> 16);
		out[2] = (uint8)(size >> 8);
		out[3] = (uint8)size;
		out.resize(4 + packedLen);
		return true;
	}

	bool Inflate(const std::vector<uint8>& stored, std::vector<uint8>& out)
	{
		out.clear();
		if (stored.size() < 4)
			return false;
		const uint32 expected = ((uint32)stored[0] << 24) | ((uint32)stored[1] << 16) | ((uint32)stored[2] << 8) | (uint32)stored[3];
		out.resize(expected);
		uLongf outLen = expected;
		uLong inLen = (uLong)(stored.size() - 4);
		const int status = uncompress2(out.data(), &outLen, stored.data() + 4, &inLen);
		if (status != Z_OK || outLen != expected || inLen != stored.size() - 4)
		{
			out.clear();
			return false;
		}
		return true;
	}

	// background writer behind AddFileAsync, started on first use
	class DeferredWriter
	{
	public:
		~DeferredWriter()
		{
			{
				std::lock_guard guard(m_lock);
				if (!m_worker.joinable())
					return;
				m_stop = true;
			}
			m_signal.notify_all();
			m_worker.join();
		}

		void Enqueue(FileCache* target, uint64 name1, uint64 name2, const uint8* data, sint32 size)
		{
			Request req{ target, name1, name2, std::vector<uint8>(data, data + size) };
			{
				std::lock_guard guard(m_lock);
				if (!m_worker.joinable())
					m_worker = std::thread(&DeferredWriter::Run, this);
				m_pending.push_back(std::move(req));
			}
			m_signal.notify_all();
		}

	private:
		struct Request
		{
			FileCache* target;
			uint64 name1;
			uint64 name2;
			std::vector<uint8> bytes;
		};

		void Run()
		{
			SetThreadName("fileCache");
			std::unique_lock guard(m_lock);
			while (true)
			{
				m_signal.wait(guard, [this] { return m_stop || !m_pending.empty(); });
				if (m_pending.empty())
					return;
				std::deque<Request> work;
				work.swap(m_pending);
				guard.unlock();
				for (Request& r : work)
					r.target->AddFile({ r.name1, r.name2 }, r.bytes.data(), (sint32)r.bytes.size());
				guard.lock();
			}
		}

		std::mutex m_lock;
		std::condition_variable m_signal;
		std::deque<Request> m_pending;
		std::thread m_worker;
		bool m_stop = false;
	};

	DeferredWriter g_deferredWriter;
}

void FileCache::WriteHeader()
{
	m_stream->SetPosition(0);
	m_stream->writeU32(kMagicCompressed);
	m_stream->writeU32(m_extraVersion);
	m_stream->writeU64(m_dataStart);
	m_stream->writeU64(m_tableOffset);
	m_stream->writeU32(m_tableSize);
}

void FileCache::WriteSlotRecord(sint32 index)
{
	m_stream->SetPosition(m_dataStart + m_tableOffset + (uint64)index * sizeof(Slot));
	m_stream->writeData(&m_slots[index], sizeof(Slot));
}

FileCache* FileCache::Create(const fs::path& path, uint32 extraVersion)
{
	FileStream* stream = FileStream::createFile2(path);
	if (!stream)
	{
		cemuLog_log(LogType::Force, "Failed to create cache file \"{}\"", _pathToUtf8(path));
		return nullptr;
	}
	FileCache* cache = new FileCache();
	cache->m_stream = stream;
	cache->m_extraVersion = extraVersion;
	cache->m_dataStart = kHeaderArea;
	cache->m_slots.assign(kInitialSlots, Slot{});
	cache->m_tableOffset = 0;
	cache->m_tableSize = (uint32)(kInitialSlots * sizeof(Slot));
	Slot& self = cache->m_slots[0];
	self.name1 = kTableName1;
	self.name2 = kTableName2;
	self.offset = cache->m_tableOffset;
	self.size = cache->m_tableSize;
	cache->WriteHeader();
	stream->SetPosition(cache->m_dataStart + cache->m_tableOffset);
	stream->writeData(cache->m_slots.data(), (sint32)cache->m_tableSize);
	return cache;
}

FileCache* FileCache::OpenWithCheck(const fs::path& path, bool checkVersion, uint32 extraVersion)
{
	FileStream* stream = FileStream::openFile2(path, true);
	if (!stream)
		return nullptr;
	uint32 magic = 0;
	stream->readU32(magic);
	if (magic != kMagicLarge && magic != kMagicCompressed) // includes kMagicLegacy
	{
		delete stream;
		return nullptr;
	}
	uint32 storedVersion = 0xFFFFFFFF;
	stream->readU32(storedVersion);
	if (checkVersion && storedVersion != extraVersion)
	{
		delete stream;
		return nullptr;
	}
	uint64 dataStart = 0;
	uint64 tableOffset = 0;
	uint32 tableSize = 0;
	stream->readU64(dataStart);
	stream->readU64(tableOffset);
	if (!stream->readU32(tableSize) || (tableSize % sizeof(Slot)) != 0)
	{
		cemuLog_log(LogType::Force, "\"{}\" is corrupted", _pathToUtf8(path));
		delete stream;
		return nullptr;
	}

	FileCache* cache = new FileCache();
	cache->m_stream = stream;
	cache->m_extraVersion = checkVersion ? extraVersion : storedVersion;
	cache->m_dataStart = dataStart;
	cache->m_tableOffset = tableOffset;
	cache->m_tableSize = tableSize;
	cache->m_slots.assign(tableSize / sizeof(Slot), Slot{});
	stream->SetPosition(dataStart + tableOffset);
	const bool complete = stream->readData(cache->m_slots.data(), tableSize) == tableSize;
	if (magic == kMagicLarge)
	{
		// the older format left the flag bytes undefined
		for (Slot& s : cache->m_slots)
		{
			s.flags = 0;
			s.reserved[0] = s.reserved[1] = s.reserved[2] = 0;
		}
	}
	if (!complete)
	{
		cemuLog_log(LogType::Force, "\"{}\" is corrupted (incomplete file table)", _pathToUtf8(path));
		delete cache;
		return nullptr;
	}
	return cache;
}

FileCache* FileCache::Open(const fs::path& path, bool allowCreate, uint32 extraVersion)
{
	if (FileCache* existing = OpenWithCheck(path, true, extraVersion))
		return existing;
	return allowCreate ? Create(path, extraVersion) : nullptr;
}

FileCache* FileCache::Open(const fs::path& path)
{
	return OpenWithCheck(path, false, 0);
}

FileCache::~FileCache()
{
	delete m_stream;
}

sint32 FileCache::FindSlot(uint64 name1, uint64 name2) const
{
	for (size_t i = 0; i < m_slots.size(); i++)
	{
		if (m_slots[i].name1 == name1 && m_slots[i].name2 == name2)
			return (sint32)i;
	}
	return -1;
}

// Enlarges the table and stores it again as a file. Slot 0 always describes
// the table itself; it is released first so the table can move anywhere.
void FileCache::GrowTable(sint32 additionalSlots)
{
	if (m_slots.empty())
		m_slots.resize(1, Slot{});
	m_slots[0].name1 = 0;
	m_slots[0].name2 = 0;
	m_slots.resize(m_slots.size() + additionalSlots, Slot{});
	Store(kTableName1, kTableName2, reinterpret_cast<const uint8*>(m_slots.data()), (sint32)(m_slots.size() * sizeof(Slot)), false);
	if (!IsTableName(m_slots[0].name1, m_slots[0].name2))
	{
		cemuLog_log(LogType::Force, "Corruption in cache file detected");
		assert_dbg();
	}
	m_tableOffset = m_slots[0].offset;
	m_tableSize = m_slots[0].size;
	WriteHeader();
}

void FileCache::Store(uint64 name1, uint64 name2, const uint8* data, sint32 size, bool allowCompression)
{
	if (size < 0)
		return;
	std::vector<uint8> packed;
	const bool compressed = allowCompression && m_compress && Deflate(data, (uint32)size, packed);
	const uint8* payload = compressed ? packed.data() : data;
	const uint32 payloadSize = compressed ? (uint32)packed.size() : (uint32)size;

	std::unique_lock guard(m_lock);
	// reuse the record of an existing file with this name, else the first unused one
	sint32 index = FindSlot(name1, name2);
	while (index < 0)
	{
		index = FindSlot(0, 0);
		if (index >= 0)
			break;
		if (IsTableName(name1, name2))
		{
			cemuLog_log(LogType::Force, "Error in cache file");
			cemu_assert_debug(false);
		}
		GrowTable(kSlotsPerGrowth);
	}

	// Lowest offset whose range [p, p + payloadSize] (end inclusive) does not
	// touch any used record's range [offset, offset + size). Records being
	// replaced still count as used. Each used record rules out the offsets
	// [offset - payloadSize, offset + size); sweep those in ascending order.
	std::vector<std::pair<sint64, sint64>> blocked;
	blocked.reserve(m_slots.size());
	for (const Slot& s : m_slots)
	{
		if (IsUnused(s.name1, s.name2))
			continue;
		blocked.emplace_back((sint64)s.offset - (sint64)payloadSize, (sint64)(s.offset + s.size));
	}
	std::sort(blocked.begin(), blocked.end());
	sint64 position = 0;
	for (const auto& [from, to] : blocked)
	{
		if (from > position)
			break;
		position = std::max(position, to);
	}

	Slot& slot = m_slots[index];
	slot.name1 = name1;
	slot.name2 = name2;
	slot.offset = (uint64)position;
	slot.size = payloadSize;
	slot.flags = compressed ? kFlagZlib : 0;
	slot.reserved[0] = slot.reserved[1] = slot.reserved[2] = 0;

	// when the table stores itself, payload points into m_slots, so the record
	// just updated above is part of the written data
	m_stream->SetPosition(m_dataStart + (uint64)position);
	m_stream->writeData(payload, (sint32)payloadSize);
#ifdef __APPLE__
	m_stream->Flush();
#endif
	WriteSlotRecord(index);
#ifdef __APPLE__
	m_stream->Flush();
#endif
}

void FileCache::AddFile(const FileName&& name, const uint8* fileData, sint32 fileSize)
{
	Store(name.name1, name.name2, fileData, fileSize, true);
}

void FileCache::AddFileAsync(const FileName& name, const uint8* fileData, sint32 fileSize)
{
	g_deferredWriter.Enqueue(this, name.name1, name.name2, fileData, fileSize);
}

bool FileCache::DeleteFile(const FileName&& name)
{
	if (IsTableName(name.name1, name.name2))
		return false;
	std::unique_lock guard(m_lock);
	const sint32 index = FindSlot(name.name1, name.name2);
	if (index < 0)
		return false;
	Slot& slot = m_slots[index];
	slot.name1 = 0;
	slot.name2 = 0;
	slot.offset = 0;
	slot.size = 0;
	WriteSlotRecord(index);
	return true;
}

bool FileCache::ReadSlot(sint32 index, std::vector<uint8>& dataOut)
{
	const Slot& slot = m_slots[index];
	std::vector<uint8> stored(slot.size);
	m_stream->SetPosition(m_dataStart + slot.offset);
	m_stream->readData(stored.data(), slot.size);
	if ((slot.flags & kFlagZlib) == 0)
	{
		dataOut = std::move(stored);
		return true;
	}
	return Inflate(stored, dataOut);
}

bool FileCache::GetFile(const FileName&& name, std::vector<uint8>& dataOut)
{
	std::unique_lock guard(m_lock);
	const sint32 index = FindSlot(name.name1, name.name2);
	if (index < 0)
	{
		dataOut.clear();
		return false;
	}
	return ReadSlot(index, dataOut);
}

bool FileCache::GetFileByIndex(sint32 index, uint64* name1, uint64* name2, std::vector<uint8>& dataOut)
{
	std::unique_lock guard(m_lock);
	if (index < 0 || index >= (sint32)m_slots.size())
		return false;
	const Slot& slot = m_slots[index];
	if (IsUnused(slot.name1, slot.name2) || IsTableName(slot.name1, slot.name2))
		return false;
	if (name1)
		*name1 = slot.name1;
	if (name2)
		*name2 = slot.name2;
	return ReadSlot(index, dataOut);
}

bool FileCache::HasFile(const FileName&& name)
{
	std::unique_lock guard(m_lock);
	return FindSlot(name.name1, name.name2) >= 0;
}

sint32 FileCache::GetMaximumFileIndex()
{
	std::unique_lock guard(m_lock);
	return (sint32)m_slots.size();
}

sint32 FileCache::GetFileCount()
{
	std::unique_lock guard(m_lock);
	sint32 count = 0;
	for (const Slot& s : m_slots)
	{
		if (!IsUnused(s.name1, s.name2) && !IsTableName(s.name1, s.name2))
			count++;
	}
	return count;
}
