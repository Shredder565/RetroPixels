// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/Filesystem/fsc.h"
#include "config/CemuConfig.h"
#include "TitleId.h"
#include "AppType.h"
#include "ParsedMetaXml.h"

// what a single file on the host looks like, judged by its header bytes
enum class CafeTitleFileType
{
	UNKNOWN,
	WUD,
	WUX,
	RPX,
	ELF,
};

CafeTitleFileType DetermineCafeSystemFileType(fs::path filePath);

// values from code/app.xml
struct ParsedAppXml
{
	uint64 title_id;
	uint16 title_version;
	uint32 app_type;
	uint32 group_id;
	uint32 sdk_version;
};

// permission groups of code/cos.xml
enum class CosCapabilityGroup : uint32
{
	None = 0,
	BSP = 1,
	DK = 3,
	USB = 9,
	UHS = 12,
	FS = 11,
	MCP = 13,
	NIM = 14,
	ACT = 15,
	FPD = 16,
	BOSS = 17,
	ACP = 18,
	PDM = 19,
	AC = 20,
	NDM = 21,
	NSEC = 22
};

enum class CosCapabilityBits : uint64
{
	All = 0xFFFFFFFFFFFFFFFFull
};

inline CosCapabilityBits operator|(CosCapabilityBits a, CosCapabilityBits b)
{
	return static_cast<CosCapabilityBits>(static_cast<uint64>(a) | static_cast<uint64>(b));
}

inline CosCapabilityBits& operator|=(CosCapabilityBits& a, CosCapabilityBits b)
{
	return a = a | b;
}

// bits of the FS permission group
enum class CosCapabilityBitsFS : uint64
{
	ODD_READ = (1llu << 0),
	ODD_WRITE = (1llu << 1),
	ODD_RAW_OPEN = (1llu << 2),
	ODD_MOUNT = (1llu << 3),
	SLCCMPT_READ = (1llu << 4),
	SLCCMPT_WRITE = (1llu << 5),
	SLCCMPT_RAW_OPEN = (1llu << 6),
	SLCCMPT_MOUNT = (1llu << 7),
	SLC_READ = (1llu << 8),
	SLC_WRITE = (1llu << 9),
	SLC_RAW_OPEN = (1llu << 10),
	SLC_MOUNT = (1llu << 11),
	MLC_READ = (1llu << 12),
	MLC_WRITE = (1llu << 13),
	MLC_RAW_OPEN = (1llu << 14),
	MLC_MOUNT = (1llu << 15),
	SDCARD_READ = (1llu << 16),
	SDCARD_WRITE = (1llu << 17),
	SDCARD_RAW_OPEN = (1llu << 18),
	SDCARD_MOUNT = (1llu << 19),
	HFIO_READ = (1llu << 20),
	HFIO_WRITE = (1llu << 21),
	HFIO_RAW_OPEN = (1llu << 22),
	HFIO_MOUNT = (1llu << 23),
	RAMDISK_READ = (1llu << 24),
	RAMDISK_WRITE = (1llu << 25),
	RAMDISK_RAW_OPEN = (1llu << 26),
	RAMDISK_MOUNT = (1llu << 27),
	USB_READ = (1llu << 28),
	USB_WRITE = (1llu << 29),
	USB_RAW_OPEN = (1llu << 30),
	USB_MOUNT = (1llu << 31),
	OTHER_READ = (1llu << 32),
	OTHER_WRITE = (1llu << 33),
	OTHER_RAW_OPEN = (1llu << 34),
	OTHER_MOUNT = (1llu << 35)
};
ENABLE_BITMASK_OPERATORS(CosCapabilityBitsFS);

// values from code/cos.xml
struct ParsedCosXml
{
  public:
	std::string argstr;

	struct Permission
	{
		CosCapabilityGroup group{CosCapabilityGroup::None};
		CosCapabilityBits mask{CosCapabilityBits::All};
	};
	Permission permissions[19]{}; // <permissions><p0>..<p18>

	static ParsedCosXml* Parse(uint8* xmlData, size_t xmlLen);

	// groups the title does not list are unrestricted
	CosCapabilityBits GetCapabilityBits(CosCapabilityGroup group) const
	{
		for (const Permission& permission : permissions)
		{
			if (permission.group == group)
				return permission.mask;
		}
		return CosCapabilityBits::All;
	}
};

// One title (base game, update, DLC or system title) at one location on the
// host, in any of the supported formats. Created either by probing a path or
// from a title cache entry (then the xml files are only read on demand).
class TitleInfo
{
  public:
	enum class TitleDataFormat
	{
		HOST_FS = 1,      // folder with code/content/meta
		WUD = 2,          // .wud/.wux disc image (also .iso)
		WIIU_ARCHIVE = 3, // .wua archive, one title per root folder
		NUS = 4,          // folder with title.tmd, title.tik and .app files
		WUHB = 5,         // Aroma homebrew bundle
		INVALID_STRUCTURE = 0,
	};

	enum class InvalidReason : uint8
	{
		NONE = 0,
		BAD_PATH_OR_INACCESSIBLE = 1,
		UNKNOWN_FORMAT = 2,
		NO_DISC_KEY = 3,
		NO_TITLE_TIK = 4,
		MISSING_XML_FILES = 4,
	};

	struct CachedInfo
	{
		TitleDataFormat titleDataFormat;
		fs::path path;
		std::string subPath; // folder inside a .wua
		uint64 titleId;
		uint16 titleVersion;
		uint32 sdkVersion;
		std::string titleName;
		CafeConsoleRegion region;
		uint32 group_id;
		uint32 app_type;
	};

	TitleInfo() {}
	TitleInfo(const fs::path& path);
	TitleInfo(const fs::path& path, std::string_view subPath); // one title inside a .wua
	TitleInfo(const CachedInfo& cachedInfo);
	~TitleInfo();

	// copies carry the parsed information but none of the mounts
	TitleInfo(const TitleInfo& other) { CopyFrom(other); }
	TitleInfo& operator=(TitleInfo other)
	{
		CopyFrom(other);
		return *this;
	}

	bool IsCached() { return m_cache != nullptr; } // made from a cache entry and not parsed since

	CachedInfo MakeCacheEntry();

	bool IsValid() const;
	InvalidReason GetInvalidReason() const { return m_invalidReason; }
	uint64 GetUID(); // hash of the title's location; not stable across sessions or installs

	fs::path GetPath() const;
	TitleDataFormat GetFormat() const { return m_format; }

	// mount a folder of the title (empty = title root) at virtualPath
	bool Mount(std::string_view virtualPath, std::string_view subfolder, sint32 mountPriority);
	void Unmount(std::string_view virtualPath);
	void UnmountAll();
	bool IsMounted() const { return !m_mounts.empty(); }

	bool ParseXmlInfo();
	bool HasValidXmlInfo() const { return m_meta && m_app && m_cos; }

	bool IsEqualByLocation(const TitleInfo& rhs) const { return m_uid == rhs.m_uid; }

	bool IsSystemDataTitle() const
	{
		if (!IsValid())
			return false;
		const uint32 type = GetAppType();
		return type == APP_TYPE::DRC_FIRMWARE || type == APP_TYPE::DRC_TEXTURE_ATLAS || type == APP_TYPE::VERSION_DATA_TITLE;
	}

	// these need parsed xml files or a cache entry
	TitleId GetAppTitleId() const;
	uint16 GetAppTitleVersion() const;
	uint32 GetAppSDKVersion() const;
	uint32 GetAppGroup() const;
	uint32 GetAppType() const;
	std::string GetMetaTitleName() const; // long name in the configured language
	CafeConsoleRegion GetMetaRegion() const;
	uint32 GetOlvAccesskey() const;

	std::string GetArgStr() const; // cos.xml argstr

	TitleIdParser::TITLE_TYPE GetTitleType();
	ParsedMetaXml* GetMetaInfo() { return m_meta; }
	ParsedCosXml* GetCosInfo() { return m_cos; }

	std::string GetPrintPath() const;   // path plus format (and .wua folder), for messages
	std::string GetInstallPath() const; // "usr/title/xxxxxxxx/xxxxxxxx" or "sys/title/..."

	static std::string GetUniqueTempMountingPath();
	// .wua root folders are named <16 hex digit title id>_v<decimal version>
	static bool ParseWuaTitleFolderName(std::string_view name, TitleId& titleIdOut, uint16& titleVersionOut);

  private:
	void CopyFrom(const TitleInfo& other);
	void FreeParsedData();
	bool DetectFormat(const fs::path& path, fs::path& pathOut, TitleDataFormat& formatOut);
	bool PickWuaBaseTitle(const fs::path& path);
	void CalcUID();
	void SetInvalidReason(InvalidReason reason);
	ParsedMetaXml* ParseAromaIni(std::span<unsigned char> content);
	bool ParseAppXml(std::vector<uint8>& appXmlData);
	void CloseContainers();

	bool m_isValid{false};
	TitleDataFormat m_format{TitleDataFormat::INVALID_STRUCTURE};
	fs::path m_fullPath;
	std::string m_subPath;
	uint64 m_uid{};
	InvalidReason m_invalidReason{InvalidReason::NONE};
	// mounts: (priority, virtual path), and the containers kept open while mounted
	std::vector<std::pair<sint32, std::string>> m_mounts;
	class FSTVolume* m_fstVolume{};
	class ZArchiveReader* m_wuaReader{};
	class WUHBReader* m_wuhbReader{};
	// xml files
	bool m_xmlParsed{false};
	ParsedMetaXml* m_meta{};
	ParsedAppXml* m_app{};
	ParsedCosXml* m_cos{};
	CachedInfo* m_cache{};
};
