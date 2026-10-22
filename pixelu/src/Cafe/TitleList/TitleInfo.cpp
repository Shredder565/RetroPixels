// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "TitleInfo.h"
#include "Cafe/Filesystem/fscDeviceHostFS.h"
#include "Cafe/Filesystem/WUHB/WUHBReader.h"
#include "Cafe/Filesystem/FST/FST.h"
#include "pugixml.hpp"
#include "Common/FileStream.h"
#include <zarchive/zarchivereader.h>
#include "util/IniParser/IniParser.h"
#include "util/crypto/crc32.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"

namespace
{
	// strtoull semantics; text that is not a number (or overflows) gives 0
	uint64 ParseUnsigned(const char* text, int base)
	{
		char* end = nullptr;
		errno = 0;
		const unsigned long long value = std::strtoull(text, &end, base);
		if (end == text || errno == ERANGE)
			return 0;
		return value;
	}

	// Several TitleInfo objects can mount titles from the same .wua. Opening an
	// archive reads its whole file tree, so open archives are shared and
	// reference counted by path.
	class SharedArchives
	{
	  public:
		ZArchiveReader* Acquire(const fs::path& path)
		{
			{
				std::lock_guard lock(m_mutex);
				if (ZArchiveReader* reader = AddRef(path))
					return reader;
			}
			// open outside of the lock, it can take a while
			ZArchiveReader* opened = ZArchiveReader::OpenFromFile(path);
			if (!opened)
				return nullptr;
			std::lock_guard lock(m_mutex);
			if (ZArchiveReader* reader = AddRef(path))
			{
				delete opened; // someone else was faster
				return reader;
			}
			m_open.emplace(path, Entry{1, opened});
			return opened;
		}

		void Release(const fs::path& path, ZArchiveReader* reader)
		{
			std::lock_guard lock(m_mutex);
			auto it = m_open.find(path);
			cemu_assert(it != m_open.end() && it->second.reader == reader);
			if (--it->second.refs == 0)
			{
				delete it->second.reader;
				m_open.erase(it);
			}
		}

	  private:
		struct Entry
		{
			uint32 refs;
			ZArchiveReader* reader;
		};

		ZArchiveReader* AddRef(const fs::path& path)
		{
			auto it = m_open.find(path);
			if (it == m_open.end())
				return nullptr;
			it->second.refs++;
			return it->second.reader;
		}

		std::mutex m_mutex;
		std::map<fs::path, Entry> m_open;
	};

	SharedArchives& Archives()
	{
		static SharedArchives s_archives;
		return s_archives;
	}

	std::atomic_uint64_t s_nextTempMountId{1};
}

// --- file type sniffing ---

CafeTitleFileType DetermineCafeSystemFileType(fs::path filePath)
{
	std::unique_ptr<FileStream> file(FileStream::openFile2(filePath));
	if (!file)
		return CafeTitleFileType::UNKNOWN;
	const uint64 size = file->GetSize();
	if (size < 32)
		return CafeTitleFileType::UNKNOWN;
	uint8 head[32]{};
	file->readData(head, sizeof(head));

	static constexpr uint8 kWuxMagic[] = {'W', 'U', 'X', '0', 0x2E, 0xD0, 0x99, 0x10};
	static constexpr uint8 kRpxMagic[] = {0x7F, 'E', 'L', 'F', 0x01, 0x02, 0x01, 0xCA, 0xFE}; // ELF32 BE, Cafe OS ABI
	static constexpr uint8 kElfMagic[] = {0x7F, 'E', 'L', 'F', 0x01, 0x02, 0x01, 0x00, 0x00}; // ELF32 BE, SysV ABI
	if (std::memcmp(head, kWuxMagic, sizeof(kWuxMagic)) == 0)
		return CafeTitleFileType::WUX;
	if (std::memcmp(head, kRpxMagic, sizeof(kRpxMagic)) == 0)
		return CafeTitleFileType::RPX;
	if (std::memcmp(head, kElfMagic, sizeof(kElfMagic)) == 0)
		return CafeTitleFileType::ELF;

	// raw disc image: starts with the product code "WUP-" and has the
	// partition table signature at 0x10000
	if (size >= 0x10000)
	{
		static constexpr uint8 kWudProductMagic[] = {'W', 'U', 'P', '-'};
		static constexpr uint8 kWudTableMagic[] = {0xCC, 0x54, 0x9E, 0xB9};
		uint8 probe[4];
		file->SetPosition(0);
		file->readData(probe, 4);
		if (std::memcmp(probe, kWudProductMagic, 4) == 0)
		{
			file->SetPosition(0x10000);
			file->readData(probe, 4);
			if (std::memcmp(probe, kWudTableMagic, 4) == 0)
				return CafeTitleFileType::WUD;
		}
	}
	return CafeTitleFileType::UNKNOWN;
}

// --- construction ---

TitleInfo::TitleInfo(const fs::path& path)
{
	m_isValid = DetectFormat(path, m_fullPath, m_format);
	if (!m_isValid)
		m_format = TitleDataFormat::INVALID_STRUCTURE;
	else
		m_isValid = ParseXmlInfo();
	if (m_isValid)
		CalcUID();
}

TitleInfo::TitleInfo(const fs::path& path, std::string_view subPath)
{
	if (!path.has_filename())
	{
		SetInvalidReason(InvalidReason::BAD_PATH_OR_INACCESSIBLE);
		return;
	}
	m_format = TitleDataFormat::WIIU_ARCHIVE;
	m_fullPath = path;
	m_subPath = subPath;
	m_isValid = true; // ParseXmlInfo requires a candidate
	m_isValid = ParseXmlInfo();
	if (m_isValid)
		CalcUID();
}

TitleInfo::TitleInfo(const CachedInfo& cachedInfo)
{
	m_cache = new CachedInfo(cachedInfo);
	m_fullPath = cachedInfo.path;
	m_subPath = cachedInfo.subPath;
	m_format = cachedInfo.titleDataFormat;
	switch (cachedInfo.titleDataFormat)
	{
	case TitleDataFormat::HOST_FS:
	case TitleDataFormat::WIIU_ARCHIVE:
	case TitleDataFormat::WUHB:
	case TitleDataFormat::WUD:
	case TitleDataFormat::NUS:
	case TitleDataFormat::INVALID_STRUCTURE:
		break;
	default:
		return;
	}
	if (cachedInfo.path.empty())
		return;
	if (cachedInfo.titleDataFormat == TitleDataFormat::WIIU_ARCHIVE && m_subPath.empty())
		return; // a .wua title always lives in a folder of the archive
	m_isValid = true;
	CalcUID();
}

TitleInfo::~TitleInfo()
{
	cemu_assert(m_mounts.empty());
	FreeParsedData();
	delete m_cache;
}

void TitleInfo::FreeParsedData()
{
	delete m_meta;
	delete m_app;
	delete m_cos;
	m_meta = nullptr;
	m_app = nullptr;
	m_cos = nullptr;
}

void TitleInfo::CopyFrom(const TitleInfo& other)
{
	if (this == &other)
		return;
	FreeParsedData();
	delete m_cache;
	m_cache = nullptr;

	m_isValid = other.m_isValid;
	m_format = other.m_format;
	m_fullPath = other.m_fullPath;
	m_subPath = other.m_subPath;
	m_uid = other.m_uid;
	m_invalidReason = other.m_invalidReason;
	m_xmlParsed = other.m_xmlParsed;
	if (other.m_meta)
		m_meta = new ParsedMetaXml(*other.m_meta);
	if (other.m_app)
		m_app = new ParsedAppXml(*other.m_app);
	if (other.m_cos)
		m_cos = new ParsedCosXml(*other.m_cos);
	if (other.m_cache)
		m_cache = new CachedInfo(*other.m_cache);
	// a copy starts out unmounted
	m_mounts.clear();
	m_fstVolume = nullptr;
	m_wuaReader = nullptr;
	m_wuhbReader = nullptr;
}

TitleInfo::CachedInfo TitleInfo::MakeCacheEntry()
{
	cemu_assert_debug(IsValid());
	CachedInfo entry;
	entry.titleDataFormat = m_format;
	entry.path = m_fullPath;
	entry.subPath = m_subPath;
	entry.titleId = GetAppTitleId();
	entry.titleVersion = GetAppTitleVersion();
	entry.sdkVersion = GetAppSDKVersion();
	entry.titleName = GetMetaTitleName();
	entry.region = GetMetaRegion();
	entry.group_id = GetAppGroup();
	entry.app_type = GetAppType();
	return entry;
}

bool TitleInfo::ParseWuaTitleFolderName(std::string_view name, TitleId& titleIdOut, uint16& titleVersionOut)
{
	// <titleid:16 hex>_v<version: decimal, no leading zeros>
	TitleId titleId;
	if (name.size() < 18 || !TitleIdParser::ParseFromStr(name, titleId))
		return false;
	if (name[16] != '_' || name[17] != 'v')
		return false;
	std::string_view digits = name.substr(18);
	if (digits.empty() || (digits.size() > 1 && digits[0] == '0'))
		return false;
	uint32 version = 0;
	for (char c : digits)
	{
		if (c < '0' || c > '9')
			return false;
		version = version * 10 + (uint32)(c - '0');
		if (version > 0xFFFF)
			return false;
	}
	titleIdOut = titleId;
	titleVersionOut = (uint16)version;
	return true;
}

// A .wua can hold many titles; a TitleInfo made from the archive path alone
// represents the first base (or system) title among the root folders.
bool TitleInfo::PickWuaBaseTitle(const fs::path& path)
{
	std::unique_ptr<ZArchiveReader> archive(ZArchiveReader::OpenFromFile(path));
	if (!archive)
		return false;
	const ZArchiveNodeHandle root = archive->LookUp("", false, true);
	const uint32 count = archive->GetDirEntryCount(root);
	for (uint32 i = 0; i < count; i++)
	{
		ZArchiveReader::DirEntry entry;
		if (!archive->GetDirEntry(root, i, entry) || !entry.isDirectory)
			continue;
		TitleId titleId;
		uint16 version;
		if (!ParseWuaTitleFolderName(entry.name, titleId, version))
			continue;
		switch (TitleIdParser(titleId).GetType())
		{
		case TitleIdParser::TITLE_TYPE::BASE_TITLE:
		case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO:
		case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE:
		case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
			m_subPath = entry.name;
			return true;
		default:
			break;
		}
	}
	return false;
}

bool TitleInfo::DetectFormat(const fs::path& path, fs::path& pathOut, TitleDataFormat& formatOut)
{
	std::error_code ec;
	if (path.has_extension() && fs::is_regular_file(path, ec))
	{
		const std::string fileName = _pathToUtf8(path.filename());
		if (boost::iends_with(fileName, ".rpx"))
		{
			// <title>/code/<name>.rpx of an extracted title
			fs::path codeFolder = path.parent_path();
			if (boost::iequals(_pathToUtf8(codeFolder.filename()), "code"))
			{
				const fs::path titleFolder = codeFolder.parent_path();
				if (fs::exists(titleFolder / "content", ec) && fs::exists(titleFolder / "meta", ec))
				{
					formatOut = TitleDataFormat::HOST_FS;
					pathOut = titleFolder;
					return true;
				}
			}
		}
		else if (boost::iends_with(fileName, ".wud") || boost::iends_with(fileName, ".wux") || boost::iends_with(fileName, ".iso"))
		{
			formatOut = TitleDataFormat::WUD;
			pathOut = path;
			return true;
		}
		else if (boost::iequals(fileName, "title.tmd"))
		{
			formatOut = TitleDataFormat::NUS;
			pathOut = path;
			return true;
		}
		else if (boost::iends_with(fileName, ".wua"))
		{
			formatOut = TitleDataFormat::WIIU_ARCHIVE;
			pathOut = path;
			return PickWuaBaseTitle(path);
		}
		else if (boost::iends_with(fileName, ".wuhb"))
		{
			std::unique_ptr<WUHBReader> reader(WUHBReader::FromPath(path));
			if (reader)
			{
				formatOut = TitleDataFormat::WUHB;
				pathOut = path;
				return true;
			}
		}
		// unknown extension: disc images can also be recognized by content
		const CafeTitleFileType type = DetermineCafeSystemFileType(path);
		if (type == CafeTitleFileType::WUD || type == CafeTitleFileType::WUX)
		{
			formatOut = TitleDataFormat::WUD;
			pathOut = path;
			return true;
		}
	}
	else if (fs::exists(path / "content", ec) && fs::exists(path / "meta", ec) && fs::exists(path / "code", ec))
	{
		// root folder of an extracted title
		formatOut = TitleDataFormat::HOST_FS;
		pathOut = path;
		return true;
	}
	SetInvalidReason(InvalidReason::UNKNOWN_FORMAT);
	return false;
}

bool TitleInfo::IsValid() const
{
	return m_isValid;
}

fs::path TitleInfo::GetPath() const
{
	if (m_isValid)
		return m_fullPath;
	cemu_assert_suspicious();
	return {};
}

void TitleInfo::CalcUID()
{
	cemu_assert_debug(m_isValid);
	uint64 uid = 0; // invalid titles have no uid
	if (m_isValid)
	{
		// hash of the absolute, normalized location (relative paths are relative to the user data folder)
		const fs::path location = m_fullPath.is_relative() ? ActiveSettings::GetUserDataPath() / m_fullPath : m_fullPath;
		uid = fs::hash_value(location.lexically_normal());
		if (m_format == TitleDataFormat::WIIU_ARCHIVE)
			uid += std::hash<std::string_view>{}(m_subPath);
	}
	m_uid = uid;
}

uint64 TitleInfo::GetUID()
{
	cemu_assert_debug(m_isValid);
	return m_uid;
}

void TitleInfo::SetInvalidReason(InvalidReason reason)
{
	// the first reason sticks
	if (m_invalidReason == InvalidReason::NONE)
		m_invalidReason = reason;
}

// --- mounting ---

bool TitleInfo::Mount(std::string_view virtualPath, std::string_view subfolder, sint32 mountPriority)
{
	cemu_assert(m_isValid);
	bool mounted = false;
	switch (m_format)
	{
	case TitleDataFormat::HOST_FS:
	{
		fs::path hostPath = m_fullPath;
		hostPath /= subfolder;
		mounted = FSCDeviceHostFS_Mount(std::string(virtualPath).c_str(), _pathToUtf8(hostPath), mountPriority);
		cemu_assert_debug(mounted);
		if (!mounted)
			SetInvalidReason(InvalidReason::BAD_PATH_OR_INACCESSIBLE);
		break;
	}
	case TitleDataFormat::WUD:
	case TitleDataFormat::NUS:
	{
		if (!m_fstVolume)
		{
			FSTVolume::ErrorCode error = FSTVolume::ErrorCode::OK;
			if (m_format == TitleDataFormat::WUD)
				m_fstVolume = FSTVolume::OpenFromDiscImage(m_fullPath, &error);
			else
				m_fstVolume = FSTVolume::OpenFromContentFolder(m_fullPath.parent_path(), &error); // m_fullPath is <folder>/title.tmd
			if (!m_fstVolume)
			{
				if (error == FSTVolume::ErrorCode::DISC_KEY_MISSING)
					SetInvalidReason(InvalidReason::NO_DISC_KEY);
				else if (error == FSTVolume::ErrorCode::TITLE_TIK_MISSING)
					SetInvalidReason(InvalidReason::NO_TITLE_TIK);
				return false;
			}
		}
		mounted = FSCDeviceWUD_Mount(virtualPath, subfolder, m_fstVolume, mountPriority);
		cemu_assert_debug(mounted);
		break;
	}
	case TitleDataFormat::WIIU_ARCHIVE:
	{
		if (!m_wuaReader)
		{
			m_wuaReader = Archives().Acquire(m_fullPath);
			if (!m_wuaReader)
				return false;
		}
		mounted = FSCDeviceWUA_Mount(virtualPath, std::string(m_subPath).append("/").append(subfolder), m_wuaReader, mountPriority);
		break;
	}
	case TitleDataFormat::WUHB:
	{
		if (!m_wuhbReader)
		{
			m_wuhbReader = WUHBReader::FromPath(m_fullPath);
			if (!m_wuhbReader)
				return false;
		}
		mounted = FSCDeviceWUHB_Mount(virtualPath, subfolder, m_wuhbReader, mountPriority);
		break;
	}
	default:
		cemu_assert_unimplemented();
		mounted = true; // keeps the old bookkeeping for an unexpected format
		break;
	}
	if (!mounted)
	{
		cemuLog_log(LogType::Force, "Mounting {} at {} failed", subfolder, virtualPath);
		if (m_mounts.empty())
			CloseContainers();
		return false;
	}
	m_mounts.emplace_back(mountPriority, std::string(virtualPath));
	return true;
}

void TitleInfo::CloseContainers()
{
	if (m_fstVolume)
	{
		delete m_fstVolume;
		m_fstVolume = nullptr;
	}
	if (m_wuaReader)
	{
		Archives().Release(m_fullPath, m_wuaReader);
		m_wuaReader = nullptr;
	}
	if (m_wuhbReader)
	{
		delete m_wuhbReader;
		m_wuhbReader = nullptr;
	}
}

void TitleInfo::Unmount(std::string_view virtualPath)
{
	auto it = std::find_if(m_mounts.begin(), m_mounts.end(), [&](const auto& m) { return m.second == virtualPath; });
	if (it == m_mounts.end())
	{
		cemu_assert_suspicious(); // not mounted there
		return;
	}
	fsc_unmount(it->second.c_str(), it->first);
	m_mounts.erase(it);
	if (m_mounts.empty())
		CloseContainers(); // last mount gone, close the image/archive
}

void TitleInfo::UnmountAll()
{
	while (!m_mounts.empty())
		Unmount(std::string(m_mounts.front().second));
}

std::string TitleInfo::GetUniqueTempMountingPath()
{
	return fmt::format("/internal/tempMount{:016x}/", s_nextTempMountId.fetch_add(1));
}

// --- xml files ---

bool TitleInfo::ParseXmlInfo()
{
	cemu_assert(m_isValid);
	if (m_xmlParsed)
		return m_isValid;
	m_xmlParsed = true;

	const std::string root = GetUniqueTempMountingPath();
	if (!Mount(root, "", FSC_PRIORITY_BASE))
		return false;
	auto readFile = [&root](std::string_view relPath) { return fsc_extractFile(fmt::format("{}{}", root, relPath).c_str()); };

	if (auto data = readFile("meta/meta.xml"))
		m_meta = ParsedMetaXml::Parse(data->data(), data->size());
	if (!m_meta)
	{
		// Aroma bundles (WUHB) describe themselves in meta/meta.ini and have no app.xml/cos.xml
		if (auto ini = readFile("meta/meta.ini"))
			m_meta = ParseAromaIni(*ini);
		if (m_meta)
		{
			m_cos = new ParsedCosXml{.argstr = "root.rpx"};
			m_app = new ParsedAppXml{m_meta->m_title_id, 0, 0, 0, 0};
		}
	}
	if (auto data = readFile("code/app.xml"))
		ParseAppXml(*data);
	if (auto data = readFile("code/cos.xml"))
	{
		delete m_cos;
		m_cos = ParsedCosXml::Parse(data->data(), data->size());
	}
	Unmount(root);

	// system data titles may come without meta.xml
	const bool metaOptional = m_app && IsSystemDataTitle();
	if ((!m_meta && !metaOptional) || !m_app || !m_cos)
	{
		if (m_meta || m_app || m_cos)
			cemuLog_log(LogType::Force, "Title is missing some of its meta/app/cos xml files: {}", _pathToUtf8(m_fullPath));
		FreeParsedData();
		m_isValid = false;
		SetInvalidReason(InvalidReason::MISSING_XML_FILES);
		return false;
	}
	m_isValid = true;
	return true;
}

ParsedMetaXml* TitleInfo::ParseAromaIni(std::span<unsigned char> content)
{
	IniParser ini{content};
	bool foundMenu = false;
	while (ini.NextSection())
	{
		if (ini.GetCurrentSectionName() == "menu")
		{
			foundMenu = true;
			break;
		}
	}
	if (!foundMenu)
		return nullptr;

	const std::string author(ini.FindOption("author").value_or(""));
	const std::string longName(ini.FindOption("longname").value_or(""));
	const std::string shortName(ini.FindOption("shortname").value_or(""));

	auto meta = std::make_unique<ParsedMetaXml>();
	const size_t en = (size_t)CafeConsoleLanguage::EN;
	meta->m_publisher[en] = author;
	meta->m_long_name[en] = longName;
	meta->m_short_name[en] = shortName;
	// homebrew has no real title id; derive a stable one in the 0005000F (homebrew) range
	const std::string idSource = author + longName + shortName;
	meta->m_title_id = (0x0005000Full << 32) | crc32_calc(idSource.data(), idSource.size());
	return meta.release();
}

bool TitleInfo::ParseAppXml(std::vector<uint8>& appXmlData)
{
	pugi::xml_document doc;
	if (!doc.load_buffer_inplace(appXmlData.data(), appXmlData.size()))
		return false;
	pugi::xml_node app = doc.child("app");
	if (!app)
		return false;
	delete m_app;
	m_app = new ParsedAppXml();
	for (pugi::xml_node node : app.children())
	{
		const std::string_view tag = node.name();
		const char* text = node.text().as_string();
		if (tag == "title_version")
			m_app->title_version = (uint16)ParseUnsigned(text, 16);
		else if (tag == "title_id")
			m_app->title_id = ParseUnsigned(text, 16);
		else if (tag == "app_type")
			m_app->app_type = (uint32)ParseUnsigned(text, 16);
		else if (tag == "group_id")
			m_app->group_id = (uint32)ParseUnsigned(text, 16);
		else if (tag == "sdk_version")
			m_app->sdk_version = (uint32)ParseUnsigned(text, 10);
	}
	return true;
}

ParsedCosXml* ParsedCosXml::Parse(uint8* xmlData, size_t xmlLen)
{
	pugi::xml_document doc;
	if (!doc.load_buffer_inplace(xmlData, xmlLen))
		return nullptr;
	pugi::xml_node app = doc.child("app");
	if (!app)
		return nullptr;
	auto cos = std::make_unique<ParsedCosXml>();
	if (pugi::xml_node argstr = app.child("argstr"))
		cos->argstr = argstr.text().as_string();
	// <permissions><p0><group>decimal</group><mask>hex</mask></p0><p1>... (stops at the first gap)
	pugi::xml_node permissions = app.child("permissions");
	for (uint32 i = 0; i < std::size(cos->permissions); i++)
	{
		pugi::xml_node entry = permissions.child(fmt::format("p{}", i).c_str());
		if (!entry)
			break;
		cos->permissions[i].group = (CosCapabilityGroup)ConvertString<uint32>(entry.child("group").text().as_string(), 10);
		cos->permissions[i].mask = (CosCapabilityBits)ConvertString<uint64>(entry.child("mask").text().as_string(), 16);
	}
	return cos.release();
}

// --- accessors ---

TitleId TitleInfo::GetAppTitleId() const
{
	cemu_assert_debug(m_isValid);
	if (m_app)
		return m_app->title_id;
	if (m_cache)
		return m_cache->titleId;
	cemu_assert_suspicious();
	return 0;
}

uint16 TitleInfo::GetAppTitleVersion() const
{
	cemu_assert_debug(m_isValid);
	if (m_app)
		return m_app->title_version;
	if (m_cache)
		return m_cache->titleVersion;
	cemu_assert_suspicious();
	return 0;
}

uint32 TitleInfo::GetAppSDKVersion() const
{
	cemu_assert_debug(m_isValid);
	if (m_app)
		return m_app->sdk_version;
	if (m_cache)
		return m_cache->sdkVersion;
	cemu_assert_suspicious();
	return 0;
}

uint32 TitleInfo::GetAppGroup() const
{
	cemu_assert_debug(m_isValid);
	if (m_app)
		return m_app->group_id;
	if (m_cache)
		return m_cache->group_id;
	cemu_assert_suspicious();
	return 0;
}

uint32 TitleInfo::GetAppType() const
{
	cemu_assert_debug(m_isValid);
	if (m_app)
		return m_app->app_type;
	if (m_cache)
		return m_cache->app_type;
	cemu_assert_suspicious();
	return 0;
}

TitleIdParser::TITLE_TYPE TitleInfo::GetTitleType()
{
	return TitleIdParser(GetAppTitleId()).GetType();
}

std::string TitleInfo::GetMetaTitleName() const
{
	cemu_assert_debug(m_isValid);
	if (m_meta)
	{
		std::string name = m_meta->GetLongName(GetConfig().console_language);
		if (name.empty())
			name = m_meta->GetLongName(CafeConsoleLanguage::EN);
		if (name.empty())
			name = "Unknown Title";
		return name;
	}
	if (m_cache)
		return m_cache->titleName;
	return {};
}

CafeConsoleRegion TitleInfo::GetMetaRegion() const
{
	cemu_assert_debug(m_isValid);
	if (m_meta)
		return m_meta->GetRegion();
	if (m_cache)
		return m_cache->region;
	return CafeConsoleRegion::JPN;
}

uint32 TitleInfo::GetOlvAccesskey() const
{
	cemu_assert_debug(m_isValid);
	if (m_meta)
		return m_meta->GetOlvAccesskey();
	cemu_assert_suspicious();
	return 0xFFFFFFFF;
}

std::string TitleInfo::GetArgStr() const
{
	cemu_assert_debug(m_cos);
	return m_cos ? m_cos->argstr : std::string();
}

std::string TitleInfo::GetPrintPath() const
{
	if (!m_isValid)
		return "invalid";
	std::string text = _pathToUtf8(m_fullPath);
	switch (m_format)
	{
	case TitleDataFormat::HOST_FS: text += " [Folder]"; break;
	case TitleDataFormat::WUD: text += " [WUD]"; break;
	case TitleDataFormat::NUS: text += " [NUS]"; break;
	case TitleDataFormat::WIIU_ARCHIVE: text += fmt::format(" [WUA] [{}]", m_subPath); break;
	case TitleDataFormat::WUHB: text += " [WUHB]"; break;
	default: break;
	}
	return text;
}

std::string TitleInfo::GetInstallPath() const
{
	const TitleId titleId = GetAppTitleId();
	const char* storage = TitleIdParser(titleId).IsSystemTitle() ? "sys" : "usr";
	return fmt::format("{}/title/{:08x}/{:08x}", storage, GetTitleIdHigh(titleId), GetTitleIdLow(titleId));
}
