// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <mutex>

// Container file for the shader, pipeline and download caches: a fixed
// header, then a data area holding the stored files and the file table (the
// table is itself kept as a stored file). Files are keyed by two 64-bit
// names and may be stored zlib compressed.
class FileCache
{
public:
	struct FileName
	{
		FileName(uint64 name1, uint64 name2) : name1(name1), name2(name2) {}

		// key derived from a path string (stored in caches, must not change)
		FileName(std::string_view filePath)
		{
			uint64 rot = 0xa2cc2c49386a75fdull;
			uint64 mul = 0x5182d367734c2ce8ull;
			for (const char ch : filePath)
			{
				const uint64 v = static_cast<uint64>(ch);
				rot = std::rotl(rot, 7) + v;
				mul = mul * 7841u + v;
			}
			name1 = rot;
			name2 = mul;
		}

		FileName(const std::string& filePath) : FileName(std::string_view(filePath)) {}

		uint64 name1;
		uint64 name2;
	};

	~FileCache();

	static FileCache* Create(const fs::path& path, uint32 extraVersion = 0);
	static FileCache* Open(const fs::path& path, bool allowCreate, uint32 extraVersion = 0);
	static FileCache* Open(const fs::path& path); // accepts any extra version

	void UseCompression(bool enable) { m_compress = enable; }

	void AddFile(const FileName&& name, const uint8* fileData, sint32 fileSize);
	void AddFileAsync(const FileName& name, const uint8* fileData, sint32 fileSize);
	bool DeleteFile(const FileName&& name);
	bool GetFile(const FileName&& name, std::vector<uint8>& dataOut);
	bool GetFileByIndex(sint32 index, uint64* name1, uint64* name2, std::vector<uint8>& dataOut);
	bool HasFile(const FileName&& name);

	sint32 GetFileCount();
	sint32 GetMaximumFileIndex();

private:
	// one file table record, exactly as stored on disk (little endian)
	struct Slot
	{
		uint64 name1;
		uint64 name2;
		uint64 offset; // relative to the data area
		uint32 size;   // stored size (compressed size if compressed)
		uint8 flags;   // bit 0: zlib compressed
		uint8 reserved[3];
	};
	static_assert(sizeof(Slot) == 0x20);

	FileCache() = default;

	static FileCache* OpenWithCheck(const fs::path& path, bool checkVersion, uint32 extraVersion);

	void Store(uint64 name1, uint64 name2, const uint8* data, sint32 size, bool allowCompression);
	void GrowTable(sint32 additionalSlots);
	bool ReadSlot(sint32 index, std::vector<uint8>& dataOut);
	sint32 FindSlot(uint64 name1, uint64 name2) const;
	void WriteSlotRecord(sint32 index);
	void WriteHeader();

	class FileStream* m_stream{};
	uint64 m_dataStart{};
	uint32 m_extraVersion{};
	std::vector<Slot> m_slots;       // the file table
	uint64 m_tableOffset{};          // as recorded in the header
	uint32 m_tableSize{};
	bool m_compress{true};
	std::recursive_mutex m_lock;
};
