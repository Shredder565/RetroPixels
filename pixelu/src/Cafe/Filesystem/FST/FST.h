// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cemu/ncrypto/ncrypto.h"

// A file or folder of an FSTVolume
struct FSTFileHandle
{
	friend class FSTVolume;

  private:
	uint32 m_fstIndex{0};
};

// Lists the direct children of a folder (see FSTVolume::OpenDirectoryIterator)
struct FSTDirectoryIterator
{
	friend class FSTVolume;

	const FSTFileHandle& GetDirHandle() const
	{
		return dirHandle;
	}

  private:
	FSTFileHandle dirHandle;
	uint32 startIndex{0};
	uint32 endIndex{0};
	uint32 currentIndex{0};
};

// The file system of a Wii U title stored as encrypted FST content: the GM
// partition of a .wud/.wux disc image, or a folder of NUS .app files with
// title.tmd and title.tik. Names are matched case-insensitively.
class FSTVolume
{
  public:
	enum class ErrorCode
	{
		OK = 0,
		UNKNOWN_ERROR = 1,
		DISC_KEY_MISSING = 2,
		TITLE_TIK_MISSING = 3,
		BAD_TITLE_TMD = 4,
		BAD_TITLE_TIK = 5,
	};

	// tries every key of keys.txt, then <image>.key next to the image
	static bool FindDiscKey(const fs::path& path, NCrypto::AesKey& discTitleKey);

	static FSTVolume* OpenFromDiscImage(const fs::path& path, NCrypto::AesKey& discTitleKey, ErrorCode* errorCodeOut = nullptr);
	static FSTVolume* OpenFromDiscImage(const fs::path& path, ErrorCode* errorCodeOut = nullptr);
	static FSTVolume* OpenFromContentFolder(fs::path folderPath, ErrorCode* errorCodeOut = nullptr);

	~FSTVolume();

	uint32 GetFileCount() const;
	bool HasCorruption() const { return m_detectedCorruption; } // set once a read failed or a hash did not match

	// an empty path opens the root folder (unless only files are wanted)
	bool OpenFile(std::string_view path, FSTFileHandle& fileHandleOut, bool openOnlyFiles = false);

	bool IsDirectory(const FSTFileHandle& fileHandle) const;
	bool IsFile(const FSTFileHandle& fileHandle) const;
	bool HasLinkFlag(const FSTFileHandle& fileHandle) const;

	std::string_view GetName(const FSTFileHandle& fileHandle) const;
	std::string GetPath(const FSTFileHandle& fileHandle) const; // relative to the root, '/' separated

	uint32 GetFileSize(const FSTFileHandle& fileHandle) const; // 0 for folders
	uint32 ReadFile(FSTFileHandle& fileHandle, uint32 offset, uint32 size, void* dataOut); // returns the number of bytes read

	bool OpenDirectoryIterator(std::string_view path, FSTDirectoryIterator& directoryIteratorOut);
	bool Next(FSTDirectoryIterator& directoryIterator, FSTFileHandle& fileHandleOut);

	// whole file; empty (and *success false) if the path is not a file
	std::vector<uint8> ExtractFile(std::string_view path, bool* success = nullptr)
	{
		if (success)
			*success = false;
		FSTFileHandle file;
		if (!OpenFile(path, file, true))
			return {};
		std::vector<uint8> data(GetFileSize(file));
		ReadFile(file, 0, (uint32)data.size(), data.data());
		if (success)
			*success = true;
		return data;
	}

  private:
	class Source;
	class DiscImageSource;
	class ContentFolderSource;
	struct Cluster;
	struct Entry;
	struct CachedBlock;

	FSTVolume();

	static FSTVolume* Parse(std::shared_ptr<Source> source, uint64 fstOffset, uint32 fstSize, const NCrypto::AesKey& key, NCrypto::TMDParser* tmd);
	static bool BuildEntries(const uint8* fileTable, uint32 entryCount, uint32 clusterCount, const std::vector<char>& names, std::vector<Entry>& entriesOut);

	sint32 FindChild(uint32 first, uint32 end, std::string_view name) const;
	bool NameEquals(const Entry& entry, std::string_view name) const;
	const char* NameOf(const Entry& entry) const;

	CachedBlock* GetRawBlock(uint32 clusterIndex, uint32 blockIndex);
	CachedBlock* GetHashedBlock(uint32 clusterIndex, uint32 blockIndex);
	void AddToCache(uint64 key, std::unique_ptr<CachedBlock> block);
	uint32 ReadRaw(const Entry& entry, uint32 offset, uint32 size, uint8* out);
	uint32 ReadHashed(const Entry& entry, uint32 offset, uint32 size, uint8* out);

	std::shared_ptr<Source> m_source;
	NCrypto::AesKey m_key{};
	uint32 m_offsetFactor{0};
	std::vector<Cluster> m_clusters;
	std::vector<Entry> m_entries;
	std::vector<char> m_names;
	bool m_detectedCorruption{false};

	std::unordered_map<uint64, std::unique_ptr<CachedBlock>> m_cache;
	size_t m_cacheBytes{0};
	uint64 m_cacheClock{0};
};

// Integrity checks for downloaded NUS content (.app files)
class FSTVerifier
{
  public:
	static bool VerifyContentFile(class FileStream* fileContent, const NCrypto::AesKey* key, uint32 contentIndex, uint32 contentSize, uint32 contentSizePadded, bool isSHA1, const uint8* tmdContentHash);
	static bool VerifyHashedContentFile(class FileStream* fileContent, const NCrypto::AesKey* key, uint32 contentIndex, uint32 contentSize, uint32 contentSizePadded, bool isSHA1, const uint8* tmdContentHash);
};
