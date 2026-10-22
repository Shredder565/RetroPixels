// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "SaveList.h"
#include <charconv>
#include "util/helpers/helpers.h"

namespace
{
	struct SaveListener
	{
		void (*callback)(CafeSaveListCallbackEvent* evt, void* ctx);
		void* context;
		uint64 id;
	};

	std::mutex s_mutex;
	fs::path s_mlcPath;
	std::vector<SaveInfo*> s_saves;
	std::vector<SaveListener> s_listeners;
	std::atomic_bool s_scanning{false};

	// caller holds s_mutex
	void Notify(CafeSaveListCallbackEvent::TYPE type, SaveInfo* saveInfo)
	{
		CafeSaveListCallbackEvent evt{type, saveInfo};
		for (const SaveListener& listener : s_listeners)
			listener.callback(&evt, listener.context);
	}

	// save folder names are hex; like std::from_chars a valid prefix is enough
	bool ParseHexFolderName(const fs::path& folder, uint32& valueOut)
	{
		const std::string name = _pathToUtf8(folder.filename());
		if (name.empty())
			return false;
		return std::from_chars(name.data(), name.data() + name.size(), valueOut, 16).ec == std::errc();
	}
}

void CafeSaveList::Initialize()
{
}

void CafeSaveList::SetMLCPath(fs::path mlcPath)
{
	std::lock_guard lock(s_mutex);
	s_mlcPath = std::move(mlcPath);
}

void CafeSaveList::Refresh()
{
	std::lock_guard lock(s_mutex);
	if (s_scanning)
		return;
	s_scanning = true;
	std::thread(RefreshThreadWorker).detach();
}

void CafeSaveList::RefreshThreadWorker()
{
	SetThreadName("SaveListWorker");
	fs::path mlcPath;
	{
		// forget everything found by the previous scan
		std::lock_guard lock(s_mutex);
		for (SaveInfo* saveInfo : s_saves)
		{
			Notify(CafeSaveListCallbackEvent::TYPE::SAVE_REMOVED, saveInfo);
			delete saveInfo;
		}
		s_saves.clear();
		mlcPath = s_mlcPath;
	}
	// usr/save/<title id high>/<title id low>
	std::error_code ec;
	for (const fs::directory_entry& highFolder : fs::directory_iterator(mlcPath / "usr/save", ec))
	{
		uint32 high;
		if (!highFolder.is_directory(ec) || !ParseHexFolderName(highFolder.path(), high))
			continue;
		for (const fs::directory_entry& lowFolder : fs::directory_iterator(highFolder.path(), ec))
		{
			uint32 low;
			if (!lowFolder.is_directory(ec) || !ParseHexFolderName(lowFolder.path(), low))
				continue;
			auto saveInfo = std::make_unique<SaveInfo>(((TitleId)high << 32) | low);
			if (saveInfo->IsValid())
				DiscoveredSave(saveInfo.release());
		}
	}
	std::lock_guard lock(s_mutex);
	s_scanning = false;
	Notify(CafeSaveListCallbackEvent::TYPE::SCAN_FINISHED, nullptr);
}

void CafeSaveList::DiscoveredSave(SaveInfo* saveInfo)
{
	std::unique_ptr<SaveInfo> owned(saveInfo);
	if (!owned->ParseMetaData())
		return; // saves without meta data are not listed
	std::lock_guard lock(s_mutex);
	const TitleId titleId = owned->GetTitleId();
	if (std::any_of(s_saves.begin(), s_saves.end(), [titleId](const SaveInfo* s) { return s->GetTitleId() == titleId; }))
		return;
	s_saves.push_back(owned.release());
	Notify(CafeSaveListCallbackEvent::TYPE::SAVE_DISCOVERED, saveInfo);
}

uint64 CafeSaveList::RegisterCallback(void (*cb)(CafeSaveListCallbackEvent* evt, void* ctx), void* ctx)
{
	static std::atomic<uint64> s_nextId{1};
	const uint64 id = s_nextId.fetch_add(1);
	std::lock_guard lock(s_mutex);
	s_listeners.push_back({cb, ctx, id});
	for (SaveInfo* saveInfo : s_saves)
	{
		CafeSaveListCallbackEvent evt{CafeSaveListCallbackEvent::TYPE::SAVE_DISCOVERED, saveInfo};
		cb(&evt, ctx);
	}
	if (!s_scanning)
		Notify(CafeSaveListCallbackEvent::TYPE::SCAN_FINISHED, nullptr); // all listeners, as before
	return id;
}

void CafeSaveList::UnregisterCallback(uint64 id)
{
	std::lock_guard lock(s_mutex);
	auto it = std::find_if(s_listeners.begin(), s_listeners.end(), [id](const SaveListener& l) { return l.id == id; });
	cemu_assert(it != s_listeners.end());
	s_listeners.erase(it);
}

SaveInfo CafeSaveList::GetSaveByTitleId(TitleId titleId)
{
	std::lock_guard lock(s_mutex);
	for (SaveInfo* saveInfo : s_saves)
	{
		if (saveInfo->GetTitleId() == titleId)
			return *saveInfo;
	}
	return {};
}
