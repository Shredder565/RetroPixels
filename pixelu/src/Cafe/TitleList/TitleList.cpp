// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "TitleList.h"
#include "Common/FileStream.h"
#include "util/helpers/helpers.h"

#include <zarchive/zarchivereader.h>

namespace
{
	struct ListenerEntry
	{
		void (*callback)(CafeTitleListCallbackEvent* evt, void* ctx);
		void* context;
		uint64 id;
	};

	// Everything below is guarded by s_mutex unless noted otherwise.
	// s_titles only holds titles with usable meta data (or cache entries for them).
	std::mutex s_mutex;
	bool s_initialized{false};
	fs::path s_cacheFile;
	std::vector<TitleInfo*> s_titles;
	std::unordered_multimap<TitleId, TitleInfo*> s_titlesById;
	// during a scan: titles known before the scan that have not been found again yet
	std::vector<TitleInfo*> s_notYetRediscovered;
	bool s_cacheNeedsSaving{false};

	fs::path s_mlcPath;
	std::vector<fs::path> s_gamePaths;

	std::thread s_scanThread;
	bool s_scanThreadRunning{false};
	std::atomic_uint32_t s_pendingScanRequests{0}; // lock free
	std::atomic_bool s_waitForScan{false};          // lock free

	std::vector<ListenerEntry> s_listeners;

	void Notify(CafeTitleListCallbackEvent::TYPE type, TitleInfo* titleInfo)
	{
		CafeTitleListCallbackEvent evt{type, titleInfo};
		for (const ListenerEntry& listener : s_listeners)
			listener.callback(&evt, listener.context);
	}

	void ForgetTitleId(TitleInfo* titleInfo)
	{
		auto [first, last] = s_titlesById.equal_range(titleInfo->GetAppTitleId());
		for (auto it = first; it != last; ++it)
		{
			if (it->second == titleInfo)
			{
				s_titlesById.erase(it);
				return;
			}
		}
		cemu_assert_suspicious();
	}

	// file names worth probing while scanning game folders (extracted titles are
	// found through their code/content/meta folders instead)
	bool IsCandidateTitleFile(const fs::path& path)
	{
		std::string extension = _pathToUtf8(path.extension());
		std::transform(extension.begin(), extension.end(), extension.begin(), _ansiToLower);
		if (extension == ".tmd")
		{
			std::string name = _pathToUtf8(path.filename());
			std::transform(name.begin(), name.end(), name.begin(), _ansiToLower);
			return name == "title.tmd";
		}
		return extension == ".wud" || extension == ".wux" || extension == ".iso" || extension == ".wua" || extension == ".wuhb";
	}

	bool IsTitleSubfolderName(std::string_view name)
	{
		return boost::iequals(name, "content") || boost::iequals(name, "code") || boost::iequals(name, "meta");
	}

	bool IsHexString(std::string_view s)
	{
		return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
	}
}

void CafeTitleList::Initialize(const fs::path cacheXmlFile)
{
	std::lock_guard lock(s_mutex);
	s_initialized = true;
	s_cacheFile = cacheXmlFile;
	LoadCacheFile();
}

// Cache file layout:
// <title_list>
//   <title titleId="0005000010101a00" version="16" sdk_version="20811" group_id="00000101" app_type="80000000">
//     <region>2</region><name>...</name><format>1</format><path>...</path>[<sub_path>...</sub_path>]
//   </title>
// </title_list>
void CafeTitleList::LoadCacheFile()
{
	s_waitForScan = true;
	cemu_assert_debug(s_initialized);
	cemu_assert_debug(s_titles.empty());
	std::optional<std::vector<uint8>> xml = FileStream::LoadIntoMemory(s_cacheFile);
	if (!xml)
		return;
	pugi::xml_document doc;
	if (!doc.load_buffer_inplace(xml->data(), xml->size(), pugi::parse_default, pugi::xml_encoding::encoding_utf8))
		return;
	for (pugi::xml_node node : doc.child("title_list"))
	{
		TitleInfo::CachedInfo entry;
		if (!TitleIdParser::ParseFromStr(node.attribute("titleId").as_string(), entry.titleId))
			continue;
		entry.titleVersion = (uint16)node.attribute("version").as_uint();
		entry.sdkVersion = node.attribute("sdk_version").as_uint();
		entry.group_id = ConvertString<uint32>(node.attribute("group_id").as_string(), 16);
		entry.app_type = ConvertString<uint32>(node.attribute("app_type").as_string(), 16);
		entry.titleDataFormat = (TitleInfo::TitleDataFormat)ConvertString<uint32>(node.child_value("format"));
		entry.region = (CafeConsoleRegion)ConvertString<uint32>(node.child_value("region"));
		entry.titleName = node.child_value("name");
		entry.path = _utf8ToPath(node.child_value("path"));
		entry.subPath = node.child_value("sub_path");

		auto titleInfo = std::make_unique<TitleInfo>(entry);
		if (!titleInfo->IsValid())
		{
			cemuLog_log(LogType::Force, "Title list cache: skipping an invalid entry");
			continue;
		}
		AddTitle(titleInfo.release());
	}
	s_waitForScan = false;
}

void CafeTitleList::StoreCacheFile()
{
	cemu_assert_debug(s_initialized);
	if (s_cacheFile.empty())
		return;
	std::lock_guard lock(s_mutex);

	pugi::xml_document doc;
	pugi::xml_node declaration = doc.append_child(pugi::node_declaration);
	declaration.append_attribute("version") = "1.0";
	declaration.append_attribute("encoding") = "UTF-8";
	pugi::xml_node list = doc.append_child("title_list");
	auto addText = [](pugi::xml_node parent, const char* name, const std::string& value) {
		parent.append_child(name).append_child(pugi::node_pcdata).set_value(value.c_str());
	};
	for (TitleInfo* titleInfo : s_titles)
	{
		const TitleInfo::CachedInfo info = titleInfo->MakeCacheEntry();
		pugi::xml_node node = list.append_child("title");
		node.append_attribute("titleId").set_value(fmt::format("{:016x}", info.titleId).c_str());
		node.append_attribute("version").set_value(fmt::format("{}", info.titleVersion).c_str());
		node.append_attribute("sdk_version").set_value(fmt::format("{}", info.sdkVersion).c_str());
		node.append_attribute("group_id").set_value(fmt::format("{:08x}", info.group_id).c_str());
		node.append_attribute("app_type").set_value(fmt::format("{:08x}", info.app_type).c_str());
		addText(node, "region", fmt::format("{}", (uint32)info.region));
		addText(node, "name", info.titleName);
		addText(node, "format", fmt::format("{}", (uint32)info.titleDataFormat));
		addText(node, "path", _pathToUtf8(info.path));
		if (!info.subPath.empty())
			addText(node, "sub_path", info.subPath);
	}

	// write a temporary file next to the cache, then replace the cache with it
	const fs::path tempFile = s_cacheFile.parent_path() / fmt::format("{}__tmp", _pathToUtf8(s_cacheFile.filename()));
	std::ofstream out(tempFile, std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.is_open())
	{
		cemuLog_log(LogType::Force, "Title list cache: cannot write {}", _pathToUtf8(tempFile));
		return;
	}
	doc.save(out, " ", pugi::format_indent, pugi::xml_encoding::encoding_utf8);
	out.close();
	std::error_code ec;
	fs::rename(tempFile, s_cacheFile, ec);
}

void CafeTitleList::ClearScanPaths()
{
	std::lock_guard lock(s_mutex);
	s_gamePaths.clear();
}

void CafeTitleList::AddScanPath(fs::path path)
{
	std::lock_guard lock(s_mutex);
	s_gamePaths.push_back(std::move(path));
}

void CafeTitleList::SetMLCPath(fs::path path)
{
	std::lock_guard lock(s_mutex);
	std::error_code ec;
	if (!fs::is_directory(path, ec))
	{
		cemuLog_log(LogType::Force, "Title list: mlc path {} is not a folder", _pathToUtf8(path));
		return;
	}
	s_mlcPath = std::move(path);
}

void CafeTitleList::Refresh()
{
	std::lock_guard lock(s_mutex);
	cemu_assert_debug(s_initialized);
	s_pendingScanRequests++;
	if (!s_scanThreadRunning)
	{
		if (s_scanThread.joinable())
			s_scanThread.join();
		s_scanThreadRunning = true;
		s_scanThread = std::thread(RefreshWorkerThread);
	}
	s_waitForScan = false;
}

bool CafeTitleList::IsScanning()
{
	std::lock_guard lock(s_mutex);
	return s_scanThreadRunning;
}

void CafeTitleList::WaitForMandatoryScan()
{
	if (!s_waitForScan)
		return;
	while (IsScanning())
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// A .wua can hold several titles; each root folder named <titleid>_v<version>
// is one title. Any other path is probed as a single title.
void CafeTitleList::AddTitleFromPath(fs::path path)
{
	if (path.has_extension() && boost::iequals(_pathToUtf8(path.extension()), ".wua"))
	{
		std::unique_ptr<ZArchiveReader> archive(ZArchiveReader::OpenFromFile(path));
		if (!archive)
		{
			cemuLog_log(LogType::Force, "{} is not a valid Wii U archive", _pathToUtf8(path));
			return;
		}
		const ZArchiveNodeHandle root = archive->LookUp("", false, true);
		cemu_assert(root != ZARCHIVE_INVALID_NODE);
		const uint32 count = archive->GetDirEntryCount(root);
		for (uint32 i = 0; i < count; i++)
		{
			ZArchiveReader::DirEntry entry;
			if (!archive->GetDirEntry(root, i, entry) || !entry.isDirectory)
				continue;
			TitleId titleId;
			uint16 version;
			if (!TitleInfo::ParseWuaTitleFolderName(entry.name, titleId, version))
			{
				cemuLog_log(LogType::Force, "{}: folder \"{}\" is not named like a title", _pathToUtf8(path), entry.name);
				continue;
			}
			auto titleInfo = std::make_unique<TitleInfo>(path, entry.name);
			if (titleInfo->IsValid())
				AddDiscoveredTitle(titleInfo.release());
		}
		return;
	}
	auto titleInfo = std::make_unique<TitleInfo>(path);
	if (titleInfo->IsValid())
		AddDiscoveredTitle(titleInfo.release());
}

bool CafeTitleList::RefreshWorkerThread()
{
	SetThreadName("TitleListWorker");
	while (s_pendingScanRequests.exchange(0) != 0)
	{
		fs::path mlcPath;
		std::vector<fs::path> gamePaths;
		{
			std::lock_guard lock(s_mutex);
			mlcPath = s_mlcPath;
			gamePaths = s_gamePaths;
			s_notYetRediscovered = s_titles;
		}
		for (const fs::path& gamePath : gamePaths)
			ScanGamePath(gamePath);
		if (!mlcPath.empty())
		{
			std::error_code ec;
			for (const fs::directory_entry& entry : fs::directory_iterator(mlcPath / "usr/title", ec))
			{
				if (entry.is_directory(ec))
					ScanMLCPath(entry.path());
			}
			ScanMLCPath(mlcPath / "sys/title/00050010");
			ScanMLCPath(mlcPath / "sys/title/00050030");
		}

		// whatever was not found again is gone
		std::lock_guard lock(s_mutex);
		if (!s_notYetRediscovered.empty())
			s_cacheNeedsSaving = true;
		for (TitleInfo* gone : s_notYetRediscovered)
		{
			ForgetTitleId(gone);
			std::erase(s_titles, gone);
		}
		for (TitleInfo* gone : s_notYetRediscovered)
		{
			Notify(CafeTitleListCallbackEvent::TYPE::TITLE_REMOVED, gone);
			delete gone;
		}
		s_notYetRediscovered.clear();
	}
	{
		std::lock_guard lock(s_mutex);
		s_scanThreadRunning = false;
		Notify(CafeTitleListCallbackEvent::TYPE::SCAN_FINISHED, nullptr);
	}
	if (s_cacheNeedsSaving)
	{
		StoreCacheFile();
		s_cacheNeedsSaving = false;
	}
	return true;
}

void CafeTitleList::ScanGamePath(const fs::path& path)
{
	std::vector<fs::path> files;
	std::vector<fs::path> folders;
	bool hasContent = false, hasCode = false, hasMeta = false;
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::directory_iterator(path, ec))
	{
		if (entry.is_regular_file(ec))
			files.push_back(entry.path());
		else if (entry.is_directory(ec))
		{
			folders.push_back(entry.path());
			const std::string name = _pathToUtf8(entry.path().filename());
			hasContent |= boost::iequals(name, "content");
			hasCode |= boost::iequals(name, "code");
			hasMeta |= boost::iequals(name, "meta");
		}
	}
	// probing files is slow, only look at known title file names
	for (const fs::path& file : files)
	{
		if (file.has_extension() && IsCandidateTitleFile(file))
			AddTitleFromPath(file);
	}
	if (hasContent && hasCode && hasMeta)
	{
		// this folder is an extracted title
		auto titleInfo = std::make_unique<TitleInfo>(path);
		if (titleInfo->IsValid())
			AddDiscoveredTitle(titleInfo.release());
		// it may still contain further titles in other subfolders
		if (folders.size() > 3)
		{
			for (const fs::path& folder : folders)
			{
				if (!IsTitleSubfolderName(_pathToUtf8(folder.filename())))
					ScanGamePath(folder);
			}
		}
	}
	else
	{
		for (const fs::path& folder : folders)
			ScanGamePath(folder);
	}
}

// mlc layout: <...>/title/<id high>/<id low>/{code,content,meta}
void CafeTitleList::ScanMLCPath(const fs::path& path)
{
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::directory_iterator(path, ec))
	{
		if (!entry.is_directory(ec))
			continue;
		const std::string name = _pathToUtf8(entry.path().filename());
		if (name.size() != 8 || !IsHexString(name))
			continue;
		if (!fs::is_directory(entry.path() / "code", ec) || !fs::is_directory(entry.path() / "content", ec) || !fs::is_directory(entry.path() / "meta", ec))
			continue;
		auto titleInfo = std::make_unique<TitleInfo>(entry.path());
		if (titleInfo->IsValid() && titleInfo->ParseXmlInfo())
			AddDiscoveredTitle(titleInfo.release());
	}
}

void CafeTitleList::AddDiscoveredTitle(TitleInfo* titleInfo)
{
	cemu_assert_debug(titleInfo->ParseXmlInfo());
	std::lock_guard lock(s_mutex);
	auto stillKnown = std::find_if(s_notYetRediscovered.begin(), s_notYetRediscovered.end(), [titleInfo](const TitleInfo* t) { return t->IsEqualByLocation(*titleInfo); });
	if (stillKnown != s_notYetRediscovered.end())
		s_notYetRediscovered.erase(stillKnown);
	AddTitle(titleInfo);
}

// takes ownership; caller holds the lock
void CafeTitleList::AddTitle(TitleInfo* titleInfo)
{
	auto sameLocation = std::find_if(s_titles.begin(), s_titles.end(), [titleInfo](const TitleInfo* t) { return t->IsEqualByLocation(*titleInfo); });
	if (sameLocation != s_titles.end())
	{
		// a parsed title replaces a cache entry for the same location, anything else is a duplicate
		if (titleInfo->IsCached() || !(*sameLocation)->IsCached())
		{
			delete titleInfo;
			return;
		}
		TitleInfo* replaced = *sameLocation;
		s_titles.erase(sameLocation);
		ForgetTitleId(replaced);
		delete replaced;
	}
	s_titles.push_back(titleInfo);
	s_titlesById.emplace(titleInfo->GetAppTitleId(), titleInfo);
	Notify(CafeTitleListCallbackEvent::TYPE::TITLE_DISCOVERED, titleInfo);
	s_cacheNeedsSaving = true;
}

uint64 CafeTitleList::RegisterCallback(void (*cb)(CafeTitleListCallbackEvent* evt, void* ctx), void* ctx)
{
	static std::atomic<uint64> s_nextId{1};
	const uint64 id = s_nextId.fetch_add(1);
	std::lock_guard lock(s_mutex);
	s_listeners.push_back({cb, ctx, id});
	for (TitleInfo* titleInfo : s_titles)
	{
		CafeTitleListCallbackEvent evt{CafeTitleListCallbackEvent::TYPE::TITLE_DISCOVERED, titleInfo};
		cb(&evt, ctx);
	}
	if (!s_scanThreadRunning)
		Notify(CafeTitleListCallbackEvent::TYPE::SCAN_FINISHED, nullptr); // (all listeners, as before)
	return id;
}

void CafeTitleList::UnregisterCallback(uint64 id)
{
	std::lock_guard lock(s_mutex);
	auto it = std::find_if(s_listeners.begin(), s_listeners.end(), [id](const ListenerEntry& e) { return e.id == id; });
	cemu_assert(it != s_listeners.end());
	s_listeners.erase(it);
}

bool CafeTitleList::HasTitle(TitleId titleId, uint16& versionOut)
{
	versionOut = 0;
	bool found = false;
	std::lock_guard lock(s_mutex);
	for (TitleInfo* titleInfo : s_titles)
	{
		if (titleInfo->GetAppTitleId() != titleId)
			continue;
		found = true;
		versionOut = std::max(versionOut, titleInfo->GetAppTitleVersion());
	}
	return found;
}

bool CafeTitleList::HasTitleAndVersion(TitleId titleId, uint16 version)
{
	std::lock_guard lock(s_mutex);
	return std::any_of(s_titles.begin(), s_titles.end(), [&](TitleInfo* t) { return t->GetAppTitleId() == titleId && t->GetAppTitleVersion() == version; });
}

std::vector<TitleId> CafeTitleList::GetAllTitleIds()
{
	std::lock_guard lock(s_mutex);
	std::vector<TitleId> result;
	std::unordered_set<TitleId> seen;
	for (TitleInfo* titleInfo : s_titles)
	{
		const TitleId titleId = titleInfo->GetAppTitleId();
		if (seen.insert(titleId).second)
			result.push_back(titleId);
	}
	return result;
}

std::span<TitleInfo*> CafeTitleList::AcquireInternalList()
{
	s_mutex.lock();
	return {s_titles.data(), s_titles.size()};
}

void CafeTitleList::ReleaseInternalList()
{
	s_mutex.unlock();
}

bool CafeTitleList::GetFirstByTitleId(TitleId titleId, TitleInfo& titleInfoOut)
{
	std::lock_guard lock(s_mutex);
	auto it = s_titlesById.find(titleId);
	if (it == s_titlesById.end())
		return false;
	titleInfoOut = *it->second;
	return true;
}

// Update and DLC ids map to the base game id. DLC is assumed to share the base
// game's id apart from the category byte.
bool CafeTitleList::FindBaseTitleId(TitleId titleId, TitleId& titleIdBaseOut)
{
	titleId = TitleIdParser::MakeBaseTitleId(titleId);
	if (((titleId >> 32) & 0xFF) == 0x0C)
		titleId &= ~(0xFFull << 32);
	titleIdBaseOut = titleId;
	return true;
}

GameInfo2 CafeTitleList::GetGameInfo(TitleId titleId)
{
	GameInfo2 gameInfo;
	TitleId baseTitleId;
	if (!FindBaseTitleId(titleId, baseTitleId))
		cemu_assert_suspicious();
	const TitleIdParser baseParser(baseTitleId);
	const bool hasUpdateId = baseParser.CanHaveSeparateUpdateTitleId();
	const TitleId updateTitleId = hasUpdateId ? baseParser.GetSeparateUpdateTitleId() : 0;

	std::lock_guard lock(s_mutex);
	for (TitleInfo* titleInfo : s_titles)
	{
		const TitleId id = titleInfo->GetAppTitleId();
		if (id == baseTitleId)
			gameInfo.SetBase(*titleInfo);
		if (hasUpdateId && id == updateTitleId)
			gameInfo.SetUpdate(*titleInfo);
	}
	if (hasUpdateId)
	{
		// DLC: same id with category 0C
		const TitleId aocTitleId = baseTitleId | (0x0Cull << 32);
		for (TitleInfo* titleInfo : s_titles)
		{
			if (titleInfo->GetAppTitleId() == aocTitleId)
				gameInfo.AddAOC(*titleInfo);
		}
	}
	return gameInfo;
}

TitleInfo CafeTitleList::GetTitleInfoByUID(uint64 uid)
{
	TitleInfo result;
	std::lock_guard lock(s_mutex);
	for (TitleInfo* titleInfo : s_titles)
	{
		if (titleInfo->GetUID() == uid)
		{
			result = *titleInfo;
			break;
		}
	}
	return result;
}
