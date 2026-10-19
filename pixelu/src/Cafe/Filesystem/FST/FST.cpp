// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "Common/FileStream.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cafe/Filesystem/WUD/wud.h"
#include "util/crypto/aes128.h"
#include "openssl/evp.h"
#include "openssl/sha.h"
#include "fstUtil.h"

#include "FST.h"
#include "KeyCache.h"

/*
	Encrypted title content as used on discs and by NUS.

	The FST ("file system table") is the first part of the data. It starts
	with a 0x20 byte header ("FST\0", offset factor, cluster count, a "hash
	disabled" byte), followed by one 0x20 byte record per cluster (sector
	offset, size, hash mode) and one 0x10 byte record per file or folder:
	  +0 type/flags (8 bits) and name offset (24 bits)
	  +4 file: data offset (in units of the offset factor); folder: parent index
	  +8 file: size; folder: index after its last descendant
	  +C permissions, +E cluster index
	Entry 0 is the root folder and its end index is the number of entries.
	The name strings follow the records.

	Each cluster is stored in one of two ways:
	  raw: AES-CBC over the whole cluster in 0x8000 byte blocks, the IV of
	       block 0 is the cluster index, later blocks chain from the
	       previous block's last 16 encrypted bytes
	  hashed: 0x10000 byte blocks of 0x400 bytes of SHA-1 hashes (IV 0)
	       and 0xFC00 bytes of data (IV = this block's H0 hash)
*/

namespace
{
	constexpr uint32 kDiscSectorSize = 0x8000;    // raw block size, also the unit of cluster offsets
	constexpr uint32 kHashedBlockSize = 0x10000;
	constexpr uint32 kHashedBlockHashes = 0x400;
	constexpr uint32 kHashedBlockData = 0xFC00;
	constexpr size_t kCacheBudget = 2 * 1024 * 1024;

	uint32 BE32(const uint8* p)
	{
		return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | (uint32)p[3];
	}

	uint16 BE16(const uint8* p)
	{
		return (uint16)((p[0] << 8) | p[1]);
	}

	void DecryptCBC(uint8* data, uint32 size, const uint8* key, const uint8* iv)
	{
		AES128_CBC_decrypt(data, data, size, key, iv);
	}

	// the hash area of a decrypted hashed block holds 16 H0, 16 H1 and 16 H2 SHA-1 hashes
	const uint8* H0Hash(const uint8* block, uint32 index)
	{
		return block + 20 * (index % 16);
	}

	const uint8* H1Hash(const uint8* block, uint32 index)
	{
		return block + 20 * 16 + 20 * (index % 16);
	}
}

// where the encrypted bytes come from
class FSTVolume::Source
{
  public:
	virtual ~Source() = default;
	// clusterOffset is where the cluster starts in a disc image (ignored for .app files)
	virtual uint64 Read(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* out, uint64 size) = 0;
};

class FSTVolume::DiscImageSource : public FSTVolume::Source
{
  public:
	explicit DiscImageSource(wud_t* image) : m_image(image) {}
	~DiscImageSource() override
	{
		wud_close(m_image);
	}

	uint64 Read(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* out, uint64 size) override
	{
		return wud_readData(m_image, out, (uint32)size, (long long)(m_partitionStart + clusterOffset + offset));
	}

	void SetPartitionStart(uint64 start)
	{
		m_partitionStart = start;
	}

  private:
	wud_t* m_image;
	uint64 m_partitionStart{0};
};

// one .app file per cluster, named after the content id from the TMD
class FSTVolume::ContentFolderSource : public FSTVolume::Source
{
  public:
	static std::shared_ptr<ContentFolderSource> Open(const fs::path& folder, NCrypto::TMDParser& tmd)
	{
		auto source = std::make_shared<ContentFolderSource>();
		for (const auto& content : tmd.GetContentList())
		{
			std::unique_ptr<FileStream> file(FileStream::openFile2(folder / fmt::format("{:08x}.app", content.contentId)));
			if (!file)
				return nullptr;
			if (source->m_files.size() <= content.index)
				source->m_files.resize((size_t)content.index + 1);
			source->m_files[content.index] = std::move(file);
		}
		return source;
	}

	uint64 Read(uint16 clusterIndex, uint64 clusterOffset, uint64 offset, void* out, uint64 size) override
	{
		if (clusterIndex >= m_files.size() || !m_files[clusterIndex])
			return 0;
		m_files[clusterIndex]->SetPosition(offset);
		return m_files[clusterIndex]->readData(out, (uint32)size);
	}

  private:
	std::vector<std::unique_ptr<FileStream>> m_files;
};

// For raw clusters with a hash in the TMD (NUS content) the blocks are hashed
// in order as they are first decrypted; the last block completes the check.
struct FSTVolume::Cluster
{
	enum class HashMode : uint8
	{
		Raw = 0,
		RawWithContentHash = 1,
		Hashed = 2,
	};
	uint32 sectorOffset{0};
	HashMode mode{HashMode::Raw};
	bool hasContentHash{false};
	bool contentHashIsSHA1{false};
	uint8 contentHash[32]{};
	uint64 contentSize{0};
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest{nullptr, &EVP_MD_CTX_free};
	uint32 blocksDigested{0};
};

struct FSTVolume::Entry
{
	uint32 nameOffset;
	uint32 parent;      // index of the parent folder (0 = root)
	uint32 dataOffset;  // files: in units of the offset factor
	uint32 sizeOrEnd;   // files: size, folders: index after the last descendant
	uint16 cluster;
	bool isFolder;
	bool isLink;
};

struct FSTVolume::CachedBlock
{
	std::vector<uint8> data;
	uint8 lastCipherBlock[16]; // raw blocks: the IV of the following block
	uint64 lastUse;
};

FSTVolume::FSTVolume() = default;

FSTVolume::~FSTVolume() = default;

bool FSTVolume::FindDiscKey(const fs::path& path, NCrypto::AesKey& discTitleKey)
{
	wud_t* image = wud_open(path);
	if (!image)
		return false;
	DiscImageSource source(image);
	// 16 bytes used as IV, then 32 bytes that decrypt to zeros with the right key
	uint8 probe[48];
	if (source.Read(0, 0, 0x18100, probe, sizeof(probe)) != sizeof(probe))
		return false;
	for (sint32 i = 0;; i++)
	{
		const uint8* key = KeyCache_GetAES128(i);
		if (!key)
			break;
		uint8 plain[32];
		std::memcpy(plain, probe + 16, sizeof(plain));
		DecryptCBC(plain, sizeof(plain), key, probe);
		if (std::all_of(std::begin(plain), std::end(plain), [](uint8 b) { return b == 0; }))
		{
			std::memcpy(discTitleKey.b, key, 16);
			return true;
		}
	}
	fs::path keyFilePath = path;
	keyFilePath.replace_extension("key");
	std::unique_ptr<FileStream> keyFile(FileStream::openFile2(keyFilePath));
	return keyFile && keyFile->readData(discTitleKey.b, sizeof(discTitleKey.b)) == sizeof(discTitleKey.b);
}

FSTVolume* FSTVolume::OpenFromDiscImage(const fs::path& path, ErrorCode* errorCodeOut)
{
	if (errorCodeOut)
		*errorCodeOut = ErrorCode::UNKNOWN_ERROR;
	KeyCache_Prepare();
	NCrypto::AesKey discKey;
	if (!FindDiscKey(path, discKey))
	{
		if (errorCodeOut)
			*errorCodeOut = ErrorCode::DISC_KEY_MISSING;
		return nullptr;
	}
	return OpenFromDiscImage(path, discKey, errorCodeOut);
}

/*
	Disc layout:
	  0x10000  magic CC549EB9
	  0x18000  partition table, encrypted with the disc key (IV 0):
	           +0 magic CCA6E67B, +4 block size (0x8000), +1C partition count,
	           +800 0x80 byte records: name (31 chars), address count, sector of the partition
	Every partition starts with a header (magic CC93A4F5) giving the sector
	and size of its FST. The SI partition's FST (disc key) contains the
	ticket of the game (GM) partition, whose title key decrypts the game.
*/
FSTVolume* FSTVolume::OpenFromDiscImage(const fs::path& path, NCrypto::AesKey& discTitleKey, ErrorCode* errorCodeOut)
{
	if (errorCodeOut)
		*errorCodeOut = ErrorCode::UNKNOWN_ERROR;
	wud_t* image = wud_open(path);
	if (!image)
		return nullptr;
	auto source = std::make_shared<DiscImageSource>(image);

	uint8 productCode[22];
	if (source->Read(0, 0, 0, productCode, sizeof(productCode)) != sizeof(productCode))
		return nullptr;
	uint8 discMagic[4];
	if (source->Read(0, 0, 0x10000, discMagic, 4) != 4 || BE32(discMagic) != 0xCC549EB9)
		return nullptr;

	std::vector<uint8> table(kDiscSectorSize);
	if (source->Read(0, 0, 0x18000, table.data(), kDiscSectorSize) != kDiscSectorSize)
		return nullptr;
	const uint8 zeroIv[16]{};
	DecryptCBC(table.data(), kDiscSectorSize, discTitleKey.b, zeroIv);
	if (BE32(table.data()) != 0xCCA6E67B)
	{
		cemuLog_log(LogType::Force, "Disc image: the partition table does not decrypt (wrong key?)");
		return nullptr;
	}
	if (BE32(table.data() + 4) != kDiscSectorSize)
	{
		cemuLog_log(LogType::Force, "Disc image: unexpected partition block size");
		return nullptr;
	}
	const uint32 partitionCount = BE32(table.data() + 0x1C);
	if (partitionCount > 30)
	{
		cemuLog_log(LogType::Force, "Disc image: too many partitions ({})", partitionCount);
		return nullptr;
	}
	constexpr uint32 kNone = 0xFFFFFFFF;
	uint32 siIndex = kNone;
	uint32 gmIndex = kNone;
	auto record = [&](uint32 i) { return table.data() + 0x800 + i * 0x80; };
	for (uint32 i = 0; i < partitionCount; i++)
	{
		const uint8* r = record(i);
		if (r[0x1F] != 1)
		{
			cemuLog_log(LogType::Force, "Disc image: partition with {} addresses is not supported", r[0x1F]);
			return nullptr;
		}
		if (r[0] == 'S' && r[1] == 'I')
		{
			if (siIndex != kNone)
			{
				cemuLog_log(LogType::Force, "Disc image: more than one SI partition");
				return nullptr;
			}
			siIndex = i;
		}
		if (r[0] == 'G' && r[1] == 'M' && gmIndex == kNone)
			gmIndex = i; // the first game partition is used
	}
	if (siIndex == kNone || gmIndex == kNone)
	{
		cemuLog_log(LogType::Force, "Disc image: SI or GM partition missing");
		return nullptr;
	}

	struct PartitionInfo
	{
		uint64 start;
		uint32 h3HashCount;
		uint32 fstSize;
		uint32 fstSector;
		uint8 fstHashType;
	};
	// the header is accepted when either its magic or its sector size is right
	auto readPartition = [&](uint32 index, PartitionInfo& info) -> bool {
		info.start = (uint64)BE32(record(index) + 0x20) * kDiscSectorSize;
		uint8 header[0x60];
		source->SetPartitionStart(0);
		if (source->Read(0, 0, info.start, header, sizeof(header)) != sizeof(header))
			return false;
		if (BE32(header) != 0xCC93A4F5 && BE32(header + 4) != kDiscSectorSize)
			return false;
		info.h3HashCount = BE32(header + 0x10);
		info.fstSize = BE32(header + 0x14);
		info.fstSector = BE32(header + 0x18);
		info.fstHashType = header[0x24];
		return true;
	};
	PartitionInfo si, gm;
	if (!readPartition(siIndex, si))
	{
		cemuLog_log(LogType::Force, "Disc image: bad SI partition header");
		return nullptr;
	}
	if (si.fstHashType == 0 && si.h3HashCount != 0)
		cemuLog_log(LogType::Force, "Disc image: unhashed SI partition lists H3 hashes");
	if (!readPartition(gmIndex, gm))
	{
		cemuLog_log(LogType::Force, "Disc image: bad GM partition header");
		return nullptr;
	}

	// the ticket of the game partition is stored in the SI partition
	source->SetPartitionStart(si.start);
	std::vector<uint8> ticketData;
	{
		std::unique_ptr<FSTVolume> siVolume(Parse(source, (uint64)si.fstSector * kDiscSectorSize, si.fstSize, discTitleKey, nullptr));
		if (!siVolume)
			return nullptr;
		ticketData = siVolume->ExtractFile(fmt::format("{:02x}/title.tik", gmIndex));
	}
	NCrypto::ETicketParser ticket;
	if (ticketData.empty() || !ticket.parse(ticketData.data(), ticketData.size()))
	{
		cemuLog_log(LogType::Force, "Disc image: bad ticket for the game partition");
		return nullptr;
	}
	NCrypto::AesKey gameKey;
	ticket.GetTitleKey(gameKey);

	source->SetPartitionStart(gm.start);
	FSTVolume* volume = Parse(source, (uint64)gm.fstSector * kDiscSectorSize, gm.fstSize, gameKey, nullptr);
	if (volume && errorCodeOut)
		*errorCodeOut = ErrorCode::OK;
	return volume;
}

FSTVolume* FSTVolume::OpenFromContentFolder(fs::path folderPath, ErrorCode* errorCodeOut)
{
	auto fail = [errorCodeOut](ErrorCode code) -> FSTVolume* {
		if (errorCodeOut)
			*errorCodeOut = code;
		return nullptr;
	};
	if (errorCodeOut)
		*errorCodeOut = ErrorCode::UNKNOWN_ERROR;
	std::optional<std::vector<uint8>> tmdData = FileStream::LoadIntoMemory(folderPath / "title.tmd");
	if (!tmdData)
		return nullptr;
	NCrypto::TMDParser tmd;
	if (!tmd.parse(tmdData->data(), tmdData->size()))
		return fail(ErrorCode::BAD_TITLE_TMD);
	std::optional<std::vector<uint8>> ticketData = FileStream::LoadIntoMemory(folderPath / "title.tik");
	if (!ticketData)
		return fail(ErrorCode::TITLE_TIK_MISSING);
	NCrypto::ETicketParser ticket;
	if (!ticket.parse(ticketData->data(), ticketData->size()))
		return fail(ErrorCode::BAD_TITLE_TIK);
	NCrypto::AesKey titleKey;
	ticket.GetTitleKey(titleKey);
	std::shared_ptr<ContentFolderSource> source = ContentFolderSource::Open(folderPath, tmd);
	if (!source)
		return nullptr;
	// the FST is the whole content with index 0
	uint32 fstSize = 0;
	for (const auto& content : tmd.GetContentList())
	{
		if (content.index == 0)
			fstSize = (uint32)content.size;
	}
	FSTVolume* volume = Parse(source, 0, fstSize, titleKey, &tmd);
	if (volume && errorCodeOut)
		*errorCodeOut = ErrorCode::OK;
	return volume;
}

// Decrypts and checks the FST. With a TMD, cluster i is matched with the
// i-th content of the TMD and unhashed clusters get verified against it.
FSTVolume* FSTVolume::Parse(std::shared_ptr<Source> source, uint64 fstOffset, uint32 fstSize, const NCrypto::AesKey& key, NCrypto::TMDParser* tmd)
{
	constexpr uint32 kHeaderSize = 0x20;
	constexpr uint32 kClusterRecordSize = 0x20;
	constexpr uint32 kEntryRecordSize = 0x10;
	if (fstSize < kHeaderSize)
		return nullptr;
	std::vector<uint8> fst(((size_t)fstSize + 15) & ~(size_t)15);
	if (source->Read(0, 0, fstOffset, fst.data(), fst.size()) != fst.size())
		return nullptr;
	const uint8 zeroIv[16]{};
	DecryptCBC(fst.data(), (uint32)fst.size(), key.b, zeroIv);

	const uint32 clusterCount = BE32(fst.data() + 8);
	if (BE32(fst.data()) != 0x46535400 || clusterCount >= 0x1000)
	{
		cemuLog_log(LogType::Force, "FST: bad header");
		return nullptr;
	}
	const uint64 entriesStart = kHeaderSize + (uint64)clusterCount * kClusterRecordSize;
	if (entriesStart + kEntryRecordSize > fstSize)
		return nullptr;
	if (tmd && tmd->GetContentList().size() != clusterCount)
	{
		cemuLog_log(LogType::Force, "FST: cluster count does not match the TMD");
		return nullptr;
	}

	std::unique_ptr<FSTVolume> volume(new FSTVolume());
	volume->m_clusters.resize(clusterCount);
	for (uint32 i = 0; i < clusterCount; i++)
	{
		const uint8* record = fst.data() + kHeaderSize + i * kClusterRecordSize;
		Cluster& cluster = volume->m_clusters[i];
		cluster.sectorOffset = BE32(record + 0);
		cluster.mode = (Cluster::HashMode)record[0x14];
		if (!tmd)
			continue;
		const auto& content = tmd->GetContentList()[i];
		cluster.hasContentHash = true;
		cluster.contentHashIsSHA1 = HAS_FLAG(content.contentFlags, NCrypto::TMDParser::TMDContentFlags::FLAG_SHA1);
		cluster.contentSize = content.size;
		std::memcpy(cluster.contentHash, content.hash32, sizeof(cluster.contentHash));
		if (cluster.mode == Cluster::HashMode::Raw || cluster.mode == Cluster::HashMode::RawWithContentHash)
		{
			cluster.digest.reset(EVP_MD_CTX_new());
			EVP_DigestInit_ex(cluster.digest.get(), cluster.contentHashIsSHA1 ? EVP_sha1() : EVP_sha256(), nullptr);
		}
	}

	const uint8* entryRecords = fst.data() + entriesStart;
	if ((entryRecords[0] & 1) == 0)
		return nullptr; // the root must be a folder
	const uint32 entryCount = BE32(entryRecords + 8);
	if (entryCount == 0 || entriesStart + (uint64)entryCount * kEntryRecordSize > fstSize)
		return nullptr;
	const uint64 namesStart = entriesStart + (uint64)entryCount * kEntryRecordSize;
	if (namesStart >= fstSize)
		return nullptr;
	volume->m_names.assign(fst.data() + namesStart, fst.data() + fstSize);
	if (!BuildEntries(entryRecords, entryCount, clusterCount, volume->m_names, volume->m_entries))
		return nullptr;

	volume->m_source = std::move(source);
	volume->m_key = key;
	volume->m_offsetFactor = BE32(fst.data() + 4);
	return volume.release();
}

// Validates the folder structure: every folder's range must lie within its
// parent's range and name its parent correctly; files must reference an
// existing cluster and every name must be terminated inside the name table.
bool FSTVolume::BuildEntries(const uint8* records, uint32 entryCount, uint32 clusterCount, const std::vector<char>& names, std::vector<Entry>& entriesOut)
{
	struct OpenFolder
	{
		uint32 index;
		uint32 end;
	};
	std::vector<OpenFolder> folders;
	folders.push_back({0, entryCount}); // stand-in parent of the root
	{
		const uint8* root = records;
		if ((root[0] & 1) == 0 || BE32(root + 8) != entryCount || BE32(root + 4) != 0)
		{
			cemuLog_log(LogType::Force, "FST: bad root entry");
			return false;
		}
	}
	entriesOut.resize(entryCount);
	for (uint32 i = 0; i < entryCount; i++)
	{
		while (i >= folders.back().end)
			folders.pop_back();
		const uint8* r = records + (size_t)i * 0x10;
		const uint8 typeAndFlags = r[0];
		const uint32 nameOffset = BE32(r) & 0xFFFFFF;
		if (std::find(names.begin() + std::min<size_t>(nameOffset, names.size()), names.end(), '\0') == names.end())
			return false;
		Entry& entry = entriesOut[i];
		entry.nameOffset = nameOffset;
		entry.parent = folders.back().index;
		entry.isLink = (typeAndFlags & 0x80) != 0;
		entry.isFolder = (typeAndFlags & 1) != 0;
		entry.dataOffset = BE32(r + 4);
		entry.sizeOrEnd = BE32(r + 8);
		entry.cluster = BE16(r + 0xE);
		if (!entry.isFolder)
		{
			if (entry.cluster >= clusterCount)
			{
				cemuLog_log(LogType::Force, "FST: file in a cluster that does not exist");
				return false;
			}
			continue;
		}
		const uint32 end = entry.sizeOrEnd;
		if (end < i || end > folders.back().end)
		{
			cemuLog_log(LogType::Force, "FST: folder range out of bounds");
			return false;
		}
		if (entry.dataOffset != folders.back().index)
		{
			cemuLog_log(LogType::Force, "FST: folder has the wrong parent");
			return false;
		}
		folders.push_back({i, end});
	}
	return true;
}

uint32 FSTVolume::GetFileCount() const
{
	return (uint32)std::count_if(m_entries.begin(), m_entries.end(), [](const Entry& e) { return !e.isFolder; });
}

const char* FSTVolume::NameOf(const Entry& entry) const
{
	return m_names.data() + entry.nameOffset;
}

bool FSTVolume::NameEquals(const Entry& entry, std::string_view name) const
{
	const char* entryName = NameOf(entry);
	const size_t len = strnlen(entryName, m_names.size() - entry.nameOffset);
	return FSCPath::MatchNodeName(std::string_view(entryName, len), name);
}

// direct children of a folder are found by skipping over the ranges of sub folders
sint32 FSTVolume::FindChild(uint32 first, uint32 end, std::string_view name) const
{
	uint32 i = first;
	while (i < end && i < m_entries.size())
	{
		const Entry& entry = m_entries[i];
		if (NameEquals(entry, name))
			return (sint32)i;
		i = entry.isFolder ? std::max(entry.sizeOrEnd, i + 1) : i + 1;
	}
	return -1;
}

bool FSTVolume::OpenFile(std::string_view path, FSTFileHandle& fileHandleOut, bool openOnlyFiles)
{
	const FSCPath parsed(path);
	if (parsed.GetNodeCount() == 0)
	{
		if (openOnlyFiles)
			return false;
		fileHandleOut.m_fstIndex = 0;
		return true;
	}
	uint32 first = 1;
	uint32 end = m_entries[0].sizeOrEnd;
	for (size_t i = 0; i + 1 < parsed.GetNodeCount(); i++)
	{
		const sint32 folder = FindChild(first, end, parsed.GetNodeName(i));
		if (folder < 0 || !m_entries[folder].isFolder)
			return false;
		first = (uint32)folder + 1;
		end = m_entries[folder].sizeOrEnd;
	}
	const sint32 found = FindChild(first, end, parsed.GetNodeName(parsed.GetNodeCount() - 1));
	if (found < 0 || (openOnlyFiles && m_entries[found].isFolder))
		return false;
	fileHandleOut.m_fstIndex = (uint32)found;
	return true;
}

bool FSTVolume::IsDirectory(const FSTFileHandle& fileHandle) const
{
	return m_entries[fileHandle.m_fstIndex].isFolder;
}

bool FSTVolume::IsFile(const FSTFileHandle& fileHandle) const
{
	return !m_entries[fileHandle.m_fstIndex].isFolder;
}

bool FSTVolume::HasLinkFlag(const FSTFileHandle& fileHandle) const
{
	return m_entries[fileHandle.m_fstIndex].isLink;
}

std::string_view FSTVolume::GetName(const FSTFileHandle& fileHandle) const
{
	if (fileHandle.m_fstIndex >= m_entries.size())
		return "";
	return NameOf(m_entries[fileHandle.m_fstIndex]);
}

std::string FSTVolume::GetPath(const FSTFileHandle& fileHandle) const
{
	const Entry& entry = m_entries[fileHandle.m_fstIndex];
	std::vector<uint32> folders; // innermost first
	for (uint32 parent = entry.parent; parent != 0; parent = m_entries[parent].parent)
		folders.push_back(parent);
	std::string path;
	for (auto it = folders.rbegin(); it != folders.rend(); ++it)
	{
		path.append(NameOf(m_entries[*it]));
		path.push_back('/');
	}
	path.append(NameOf(entry));
	return path;
}

uint32 FSTVolume::GetFileSize(const FSTFileHandle& fileHandle) const
{
	const Entry& entry = m_entries[fileHandle.m_fstIndex];
	return entry.isFolder ? 0 : entry.sizeOrEnd;
}

uint32 FSTVolume::ReadFile(FSTFileHandle& fileHandle, uint32 offset, uint32 size, void* dataOut)
{
	const Entry& entry = m_entries[fileHandle.m_fstIndex];
	if (entry.isFolder)
		return 0;
	switch (m_clusters[entry.cluster].mode)
	{
	case Cluster::HashMode::Raw:
	case Cluster::HashMode::RawWithContentHash:
		return ReadRaw(entry, offset, size, (uint8*)dataOut);
	case Cluster::HashMode::Hashed:
		return ReadHashed(entry, offset, size, (uint8*)dataOut);
	default:
		return 0;
	}
}

bool FSTVolume::OpenDirectoryIterator(std::string_view path, FSTDirectoryIterator& directoryIteratorOut)
{
	FSTFileHandle folder;
	if (!OpenFile(path, folder, false) || !IsDirectory(folder))
		return false;
	directoryIteratorOut.dirHandle = folder;
	directoryIteratorOut.startIndex = folder.m_fstIndex + 1;
	directoryIteratorOut.endIndex = m_entries[folder.m_fstIndex].sizeOrEnd;
	directoryIteratorOut.currentIndex = directoryIteratorOut.startIndex;
	return true;
}

bool FSTVolume::Next(FSTDirectoryIterator& directoryIterator, FSTFileHandle& fileHandleOut)
{
	const uint32 index = directoryIterator.currentIndex;
	if (index >= directoryIterator.endIndex || index >= m_entries.size())
		return false;
	const Entry& entry = m_entries[index];
	fileHandleOut.m_fstIndex = index;
	directoryIterator.currentIndex = entry.isFolder ? std::max(entry.sizeOrEnd, index + 1) : index + 1;
	return true;
}

// Decrypted blocks are kept in a small least-recently-used cache.
void FSTVolume::AddToCache(uint64 key, std::unique_ptr<CachedBlock> block)
{
	while (m_cacheBytes >= kCacheBudget && !m_cache.empty())
	{
		auto oldest = std::min_element(m_cache.begin(), m_cache.end(), [](const auto& a, const auto& b) { return a.second->lastUse < b.second->lastUse; });
		m_cacheBytes -= oldest->second->data.size();
		m_cache.erase(oldest);
	}
	m_cacheBytes += block->data.size();
	m_cache[key] = std::move(block);
}

static uint64 BlockKey(uint32 clusterIndex, uint32 blockIndex, bool hashed)
{
	return ((uint64)clusterIndex << 48) | ((uint64)hashed << 47) | blockIndex;
}

FSTVolume::CachedBlock* FSTVolume::GetRawBlock(uint32 clusterIndex, uint32 blockIndex)
{
	const uint64 key = BlockKey(clusterIndex, blockIndex, false);
	if (auto it = m_cache.find(key); it != m_cache.end())
	{
		it->second->lastUse = ++m_cacheClock;
		return it->second.get();
	}
	Cluster& cluster = m_clusters[clusterIndex];
	const uint64 clusterStart = (uint64)cluster.sectorOffset * kDiscSectorSize;
	auto block = std::make_unique<CachedBlock>();
	block->data.resize(kDiscSectorSize);
	if (m_source->Read((uint16)clusterIndex, clusterStart, (uint64)blockIndex * kDiscSectorSize, block->data.data(), kDiscSectorSize) != kDiscSectorSize)
	{
		cemuLog_log(LogType::Force, "FST: cannot read raw block {} of cluster {}", blockIndex, clusterIndex);
		m_detectedCorruption = true;
		return nullptr;
	}
	// CBC chaining: block 0 starts from the cluster index, later blocks from the previous ciphertext
	uint8 iv[16]{};
	if (blockIndex == 0)
	{
		iv[0] = (uint8)(clusterIndex >> 8);
		iv[1] = (uint8)clusterIndex;
	}
	else if (auto prev = m_cache.find(BlockKey(clusterIndex, blockIndex - 1, false)); prev != m_cache.end())
		std::memcpy(iv, prev->second->lastCipherBlock, 16);
	else if (m_source->Read((uint16)clusterIndex, clusterStart, (uint64)blockIndex * kDiscSectorSize - 16, iv, 16) != 16)
	{
		cemuLog_log(LogType::Force, "FST: cannot read the IV of raw block {} of cluster {}", blockIndex, clusterIndex);
		m_detectedCorruption = true;
		std::memset(iv, 0, sizeof(iv));
	}
	std::memcpy(block->lastCipherBlock, block->data.data() + kDiscSectorSize - 16, 16);
	DecryptCBC(block->data.data(), kDiscSectorSize, m_key.b, iv);

	if (cluster.hasContentHash && cluster.digest && cluster.blocksDigested == blockIndex)
	{
		const uint64 blockCount = std::max<uint64>(cluster.contentSize / kDiscSectorSize, 1);
		const bool lastBlock = (uint64)blockIndex + 1 == blockCount;
		uint64 digestSize = kDiscSectorSize;
		if (lastBlock)
			digestSize = std::min<uint64>(cluster.contentSize - (uint64)blockIndex * kDiscSectorSize, kDiscSectorSize);
		EVP_DigestUpdate(cluster.digest.get(), block->data.data(), (size_t)digestSize);
		cluster.blocksDigested++;
		if (lastBlock)
		{
			uint8 digest[EVP_MAX_MD_SIZE];
			EVP_DigestFinal_ex(cluster.digest.get(), digest, nullptr);
			if (std::memcmp(digest, cluster.contentHash, cluster.contentHashIsSHA1 ? 20 : 32) != 0)
			{
				cemuLog_log(LogType::Force, "FST: content hash mismatch in cluster {}", clusterIndex);
				m_detectedCorruption = true;
				return nullptr;
			}
		}
	}
	block->lastUse = ++m_cacheClock;
	CachedBlock* result = block.get();
	AddToCache(key, std::move(block));
	return result;
}

FSTVolume::CachedBlock* FSTVolume::GetHashedBlock(uint32 clusterIndex, uint32 blockIndex)
{
	const uint64 key = BlockKey(clusterIndex, blockIndex, true);
	if (auto it = m_cache.find(key); it != m_cache.end())
	{
		it->second->lastUse = ++m_cacheClock;
		return it->second.get();
	}
	const uint64 clusterStart = (uint64)m_clusters[clusterIndex].sectorOffset * kDiscSectorSize;
	auto block = std::make_unique<CachedBlock>();
	block->data.resize(kHashedBlockSize);
	uint8* raw = block->data.data();
	if (m_source->Read((uint16)clusterIndex, clusterStart, (uint64)blockIndex * kHashedBlockSize, raw, kHashedBlockSize) != kHashedBlockSize)
	{
		cemuLog_log(LogType::Force, "FST: cannot read hashed block {} of cluster {}", blockIndex, clusterIndex);
		m_detectedCorruption = true;
		return nullptr;
	}
	const uint8 zeroIv[16]{};
	DecryptCBC(raw, kHashedBlockHashes, m_key.b, zeroIv);
	DecryptCBC(raw + kHashedBlockHashes, kHashedBlockData, m_key.b, H0Hash(raw, blockIndex));
	uint8 sha1[20];
	SHA1(raw + kHashedBlockHashes, kHashedBlockData, sha1);
	if (std::memcmp(sha1, H0Hash(raw, blockIndex), 20) != 0)
	{
		cemuLog_log(LogType::Force, "FST: H0 hash mismatch in block {} of cluster {}", blockIndex, clusterIndex);
		m_detectedCorruption = true;
		return nullptr;
	}
	block->lastUse = ++m_cacheClock;
	CachedBlock* result = block.get();
	AddToCache(key, std::move(block));
	return result;
}

// stops at the end of the file; returns what could be read before a failing block
uint32 FSTVolume::ReadRaw(const Entry& entry, uint32 offset, uint32 size, uint8* out)
{
	const uint32 fileSize = entry.sizeOrEnd;
	if (offset >= fileSize)
		return 0;
	size = (uint32)std::min<uint64>(size, (uint64)fileSize - offset);
	uint64 position = (uint64)entry.dataOffset * m_offsetFactor + offset;
	uint32 done = 0;
	while (done < size)
	{
		const CachedBlock* block = GetRawBlock(entry.cluster, (uint32)(position / kDiscSectorSize));
		if (!block)
			break;
		const uint32 inBlock = (uint32)(position % kDiscSectorSize);
		const uint32 chunk = std::min(size - done, kDiscSectorSize - inBlock);
		std::memcpy(out + done, block->data.data() + inBlock, chunk);
		done += chunk;
		position += chunk;
	}
	return done;
}

// not limited to the file size (callers do that); any failing block fails the whole read
uint32 FSTVolume::ReadHashed(const Entry& entry, uint32 offset, uint32 size, uint8* out)
{
	const uint64 position = (uint64)entry.dataOffset * m_offsetFactor + offset;
	uint32 blockIndex = (uint32)(position / kHashedBlockData);
	uint32 inBlock = (uint32)(position % kHashedBlockData);
	uint32 done = 0;
	while (done < size)
	{
		const CachedBlock* block = GetHashedBlock(entry.cluster, blockIndex);
		if (!block)
			return 0;
		const uint32 chunk = std::min(size - done, kHashedBlockData - inBlock);
		std::memcpy(out + done, block->data.data() + kHashedBlockHashes + inBlock, chunk);
		done += chunk;
		blockIndex++;
		inBlock = 0;
	}
	return done;
}

// Unhashed content: AES-CBC over the whole file (IV from the content index),
// hashed over the content size with SHA-1 or SHA-256.
bool FSTVerifier::VerifyContentFile(FileStream* fileContent, const NCrypto::AesKey* key, uint32 contentIndex, uint32 contentSize, uint32 contentSizePadded, bool isSHA1, const uint8* tmdContentHash)
{
	if ((uint32)fileContent->GetSize() != contentSizePadded)
		return false;
	fileContent->SetPosition(0);
	uint8 iv[16]{};
	iv[0] = (uint8)(contentIndex >> 8);
	iv[1] = (uint8)contentIndex;
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
	EVP_DigestInit(digest.get(), isSHA1 ? EVP_sha1() : EVP_sha256());
	std::vector<uint8> buffer(64 * 1024);
	for (uint64 remaining = contentSize; remaining > 0;)
	{
		const uint32 chunk = (uint32)std::min<uint64>(remaining, buffer.size());
		const uint32 chunkPadded = (chunk + 15) & ~15u;
		if (fileContent->readData(buffer.data(), chunkPadded) != chunkPadded)
			return false;
		AES128_CBC_decrypt_updateIV(buffer.data(), buffer.data(), chunkPadded, key->b, iv);
		EVP_DigestUpdate(digest.get(), buffer.data(), chunk);
		remaining -= chunk;
	}
	uint8 hash[EVP_MAX_MD_SIZE];
	unsigned int hashSize = 0;
	EVP_DigestFinal_ex(digest.get(), hash, &hashSize);
	return std::memcmp(hash, tmdContentHash, hashSize) == 0;
}

// Hashed content: checks every block's data against its H0 hash and every
// group of 16 H0 hashes against the H1 hash (H2..H4 are not checked).
bool FSTVerifier::VerifyHashedContentFile(FileStream* fileContent, const NCrypto::AesKey* key, uint32 contentIndex, uint32 contentSize, uint32 contentSizePadded, bool isSHA1, const uint8* tmdContentHash)
{
	if (!isSHA1)
		return false;
	if (contentSize % kHashedBlockSize != 0 || (uint32)fileContent->GetSize() != contentSize)
		return false;
	fileContent->SetPosition(0);
	std::vector<uint8> block(kHashedBlockSize);
	std::vector<uint8> h0Hashes(4096 * 20);
	const uint8 zeroIv[16]{};
	for (uint32 blockIndex = 0; blockIndex < contentSize / kHashedBlockSize; blockIndex++)
	{
		if (fileContent->readData(block.data(), kHashedBlockSize) != kHashedBlockSize)
			return false;
		DecryptCBC(block.data(), kHashedBlockHashes, key->b, zeroIv);
		DecryptCBC(block.data() + kHashedBlockHashes, kHashedBlockData, key->b, H0Hash(block.data(), blockIndex));
		const uint32 h0Index = blockIndex % 4096;
		uint8* h0 = h0Hashes.data() + h0Index * 20;
		SHA1(block.data() + kHashedBlockHashes, kHashedBlockData, h0);
		if (std::memcmp(h0, H0Hash(block.data(), blockIndex), 20) != 0)
			return false;
		if (h0Index % 16 == 15)
		{
			const uint32 h1Index = h0Index / 16;
			uint8 h1[20];
			SHA1(h0Hashes.data() + h1Index * 16 * 20, 16 * 20, h1);
			if (std::memcmp(h1, H1Hash(block.data(), h1Index), 20) != 0)
				return false;
		}
	}
	return true;
}

void FSTVolumeTest()
{
	FSTPathUnitTest();
}
