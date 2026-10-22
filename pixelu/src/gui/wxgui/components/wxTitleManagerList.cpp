// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/components/wxTitleManagerList.h"
#include "wxgui/helpers/wxHelpers.h"
#include "util/helpers/SystemException.h"
#include "Cafe/TitleList/GameInfo.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "wxgui/components/wxGameList.h"
#include "wxgui/helpers/wxCustomEvents.h"

#include <wx/imaglist.h>
#include <wx/wupdlock.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include "../wxHelper.h"

#include <functional>

#include "config/ActiveSettings.h"
#include "wxgui/ChecksumTool.h"
#include "wxgui/MainWindow.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/SaveList.h"

#include <zarchive/zarchivewriter.h>
#include <zarchive/zarchivereader.h>

#include "Common/FileStream.h"

// ==== titlesort begin ====
// Display order of the title manager rows (no wx types, so it can be tested on its own).
namespace titlesort
{
	enum Column
	{
		ByPath = -1,
		ByTitleId = 0,
		ByName = 1,
		ByType = 2,
		ByVersion = 3,
		ByRegion = 4,
		ByFormat = 5,
	};

	template<typename T>
	int Three(const T& a, const T& b)
	{
		return a < b ? -1 : (b < a ? 1 : 0);
	}

	// three-way comparison of two entries; ties fall through to further keys so the order is total.
	// nameCmp(a, b) compares names case-insensitively
	template<typename TEntry, typename TNameCmp>
	int Compare(int column, const TEntry& a, const TEntry& b, TNameCmp nameCmp)
	{
		auto byPath = [&] { return a.path.compare(b.path); };
		auto byType = [&] {
			const int c = Three((int)a.type, (int)b.type);
			return c != 0 ? c : byPath();
		};
		auto byTitleId = [&] {
			const int c = Three(a.title_id, b.title_id);
			return c != 0 ? c : byType();
		};
		int c = 0;
		switch (column)
		{
		case ByTitleId: return byTitleId();
		case ByType: return byType();
		case ByName: c = nameCmp(a, b); return c != 0 ? c : byTitleId();
		case ByVersion: c = Three(a.version, b.version); return c != 0 ? c : byTitleId();
		case ByRegion: c = Three((int)a.region, (int)b.region); return c != 0 ? c : byTitleId();
		case ByFormat: c = Three((int)a.format, (int)b.format); return c != 0 ? c : byType();
		case ByPath: return byPath();
		default: return 0;
		}
	}

	// strict "a before b": visible rows first (except for the plain path order), then by column
	template<typename TEntry, typename TNameCmp>
	bool Before(int column, bool visibleA, const TEntry& a, bool visibleB, const TEntry& b, TNameCmp nameCmp)
	{
		if (column != ByPath && visibleA != visibleB)
			return visibleA;
		return Compare(column, a, b, nameCmp) < 0;
	}
}
// ==== titlesort end ====

namespace
{
	wxDEFINE_EVENT(wxEVT_TITLE_FOUND, wxCommandEvent);
	wxDEFINE_EVENT(wxEVT_TITLE_REMOVED, wxCommandEvent);

	enum
	{
		ID_OPEN_DIRECTORY = wxID_HIGHEST + 1,
		ID_DELETE,
		ID_LAUNCH,
		ID_VERIFY,
		ID_CONVERT_WUA,
	};

	wxTitleManagerList::EntryType TypeOf(TitleInfo& title)
	{
		using T = wxTitleManagerList::EntryType;
		switch (title.GetTitleType())
		{
		case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE: return T::Update;
		case TitleIdParser::TITLE_TYPE::AOC: return T::Dlc;
		case TitleIdParser::TITLE_TYPE::SYSTEM_DATA:
		case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
		case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE: return T::System;
		default: return T::Base;
		}
	}

	wxTitleManagerList::EntryFormat FormatOf(TitleInfo& title)
	{
		using F = wxTitleManagerList::EntryFormat;
		switch (title.GetFormat())
		{
		case TitleInfo::TitleDataFormat::WUD: return F::WUD;
		case TitleInfo::TitleDataFormat::NUS: return F::NUS;
		case TitleInfo::TitleDataFormat::WIIU_ARCHIVE: return F::WUA;
		case TitleInfo::TitleDataFormat::WUHB: return F::WUHB;
		default: return F::Folder;
		}
	}

	// long names may span two lines
	wxString DisplayName(ParsedMetaXml* meta)
	{
		std::string name = meta->GetLongName(GetConfig().console_language.GetValue());
		if (const size_t nl = name.find('\n'); nl != std::string::npos)
			name.replace(nl, 1, " - ");
		return wxString::FromUTF8(name);
	}

	void PostEntry(wxEvtHandler* target, wxEventType type, const wxTitleManagerList::TitleEntry& entry)
	{
		auto* evt = new wxCommandEvent(type);
		evt->SetClientObject(new wxCustomData<wxTitleManagerList::TitleEntry>(entry));
		wxQueueEvent(target, evt);
	}

	// writes the files of mounted titles into a .wua archive; runs on a worker thread
	class WuaBuilder
	{
	public:
		explicit WuaBuilder(const fs::path& output)
			: m_output(output)
		{
			m_ok = true;
			m_writer = new ZArchiveWriter(&WuaBuilder::OnNewFile, &WuaBuilder::OnWrite, this);
		}

		~WuaBuilder()
		{
			delete m_file;
			delete m_writer;
		}

		bool Opened() const { return m_ok; }

		bool Build(std::vector<TitleInfo*> titles)
		{
			m_filesDone = 0;
			m_fileCount = 0;
			// first pass only sizes things up for the progress display
			for (TitleInfo* t : titles)
			{
				if (!WithMounted(t, [this](const std::string& root) { return Count(root); }) || cancel)
					return false;
			}
			for (TitleInfo* t : titles)
			{
				const std::string folder = fmt::format("{:016x}_v{}/", t->GetAppTitleId(), t->GetAppTitleVersion());
				if (!WithMounted(t, [&](const std::string& root) { return Store(folder, root); }))
					return false;
			}
			return true;
		}

		void Finish()
		{
			m_writer->Finalize();
			CloseFile();
		}

		void CloseFile()
		{
			delete m_file;
			m_file = nullptr;
		}

		std::atomic_bool cancel{false};
		std::atomic_uint32_t m_fileCount{};
		std::atomic_uint32_t m_filesDone{};
		std::atomic_uint64_t m_bytesTotal{};
		std::atomic_uint64_t m_bytesDone{};

	private:
		static void OnNewFile(const int32_t /*partIndex*/, void* ctx)
		{
			auto* self = (WuaBuilder*)ctx;
			self->m_file = FileStream::createFile2(self->m_output);
			if (self->m_file == nullptr)
				self->m_ok = false;
		}

		static void OnWrite(const void* data, size_t length, void* ctx)
		{
			auto* self = (WuaBuilder*)ctx;
			if (self->m_file != nullptr)
				self->m_file->writeData(data, length);
		}

		template<typename TFn>
		static bool WithMounted(TitleInfo* title, TFn fn)
		{
			const std::string root = TitleInfo::GetUniqueTempMountingPath();
			title->Mount(root.c_str(), "", FSC_PRIORITY_BASE);
			const bool ok = fn(root);
			title->Unmount(root.c_str());
			return ok;
		}

		bool Count(const std::string& dir)
		{
			sint32 status;
			std::unique_ptr<FSCVirtualFile> it(fsc_openDirIterator(dir.c_str(), &status));
			if (!it || cancel)
				return false;
			FSCDirEntry e;
			while (fsc_nextDir(it.get(), &e))
			{
				if (e.isFile)
				{
					m_bytesTotal += (uint64)e.fileSize;
					m_fileCount++;
				}
				else if (e.isDirectory)
				{
					if (!Count(fmt::format("{}{}/", dir, e.path)))
						return false;
				}
				else
					cemu_assert_unimplemented();
			}
			return true;
		}

		bool Store(const std::string& archiveDir, const std::string& dir)
		{
			sint32 status;
			std::unique_ptr<FSCVirtualFile> it(fsc_openDirIterator(dir.c_str(), &status));
			if (!it || cancel)
				return false;
			m_writer->MakeDir(archiveDir.c_str(), false);
			FSCDirEntry e;
			while (fsc_nextDir(it.get(), &e))
			{
				if (e.isDirectory)
				{
					if (!Store(fmt::format("{}{}/", archiveDir, e.path), fmt::format("{}{}/", dir, e.path)))
						return false;
					continue;
				}
				if (!e.isFile)
				{
					cemu_assert_unimplemented();
					continue;
				}
				m_writer->StartNewFile((archiveDir + e.path).c_str());
				std::unique_ptr<FSCVirtualFile> in(fsc_open((dir + e.path).c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status));
				if (!in)
					return false;
				m_buffer.resize(32 * 1024);
				while (const uint32 got = in->fscReadData(m_buffer.data(), (uint32)m_buffer.size()))
				{
					m_writer->AppendData(m_buffer.data(), got);
					if (cancel)
						return false;
					m_bytesDone += got;
				}
				m_filesDone++;
			}
			return true;
		}

		fs::path m_output;
		FileStream* m_file = nullptr;
		ZArchiveWriter* m_writer = nullptr;
		bool m_ok = false;
		std::vector<uint8> m_buffer;
	};
}

// ---------------------------------------------------------------------------

wxTitleManagerList::wxTitleManagerList(wxWindow* parent, wxWindowID id)
	: wxListView(parent, id, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL)
{
	struct ColumnSpec
	{
		ItemColumn col;
		wxString label;
		int width;
	};
	const ColumnSpec columns[] = {
		{ColumnTitleId, _("Title ID"), 120},
		{ColumnName, _("Name"), 435},
		{ColumnType, _("Type"), 65},
		{ColumnVersion, _("Version"), 40},
		{ColumnRegion, _("Region"), 60},
		{ColumnFormat, _("Format"), 63},
		{ColumnLocation, _("Location"), 63},
	};
	for (const ColumnSpec& c : columns)
	{
		wxListItem item;
		item.SetId(c.col);
		item.SetText(c.label);
		item.SetWidth(c.width);
		InsertColumn(c.col, item);
	}

	Bind(wxEVT_LIST_COL_CLICK, [this](wxListEvent& evt) {
		SortEntries(evt.GetColumn());
		evt.Skip();
	});
	Bind(wxEVT_CONTEXT_MENU, &wxTitleManagerList::OnContextMenu, this);
	Bind(wxEVT_REMOVE_ITEM, [this](wxCommandEvent& evt) { RemoveRow(evt.GetInt()); });
	Bind(wxEVT_TITLE_FOUND, &wxTitleManagerList::OnTitleFoundEvent, this);
	Bind(wxEVT_TITLE_REMOVED, &wxTitleManagerList::OnTitleRemovedEvent, this);
	Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) {
		evt.Skip();
		// let a running delete finish first
		if (m_context_worker.valid())
			m_context_worker.get();
	});

	m_callbackIdTitleList = CafeTitleList::RegisterCallback([](CafeTitleListCallbackEvent* evt, void* ctx) { ((wxTitleManagerList*)ctx)->QueueTitleEvent(evt); }, this);
	m_callbackIdSaveList = CafeSaveList::RegisterCallback([](CafeSaveListCallbackEvent* evt, void* ctx) { ((wxTitleManagerList*)ctx)->QueueSaveEvent(evt); }, this);

	ShowSortIndicator(ColumnTitleId);
}

wxTitleManagerList::~wxTitleManagerList()
{
	CafeSaveList::UnregisterCallback(m_callbackIdSaveList);
	CafeTitleList::UnregisterCallback(m_callbackIdTitleList);
}

// ---------------------------------------------------------------------------
// entries

boost::optional<wxTitleManagerList::TitleEntry&> wxTitleManagerList::EntryAtRow(long row) const
{
	if (row < 0)
		return {};
	long visibleIndex = 0;
	for (const auto& ref : m_sorted)
	{
		if (!ref.get().visible)
			continue;
		if (visibleIndex++ == row)
			return ref.get().entry;
	}
	return {};
}

boost::optional<wxTitleManagerList::TitleEntry&> wxTitleManagerList::EntryByUid(uint64 uid) const
{
	for (const auto& item : m_data)
	{
		if (item->entry.location_uid == uid)
			return item->entry;
	}
	return {};
}

boost::optional<const wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetSelectedTitleEntry() const
{
	if (auto e = EntryAtRow(GetFirstSelected()))
		return *e;
	return {};
}

boost::optional<wxTitleManagerList::TitleEntry&> wxTitleManagerList::GetSelectedTitleEntry()
{
	return EntryAtRow(GetFirstSelected());
}

void wxTitleManagerList::AddTitle(TitleEntryData_t* obj)
{
	const TitleEntry& entry = obj->GetData();
	if (EntryByUid(entry.location_uid))
		return; // known already
	m_data.emplace_back(std::make_unique<ItemData>(true, entry));
	m_sorted.emplace_back(*m_data.back());
	SetItemCount(m_data.size());
}

void wxTitleManagerList::RemoveRow(long row)
{
	const long rows = GetItemCount();
	long visibleIndex = 0;
	const ItemData* victim = nullptr;
	for (auto it = m_sorted.begin(); it != m_sorted.end(); ++it)
	{
		if (!it->get().visible || visibleIndex++ != row)
			continue;
		victim = &it->get();
		m_sorted.erase(it);
		break;
	}
	if (victim == nullptr)
		return;
	std::erase_if(m_data, [victim](const std::unique_ptr<ItemData>& p) { return p.get() == victim; });
	ShowRows(std::max(0L, rows - 1));
}

void wxTitleManagerList::RemoveEntry(const TitleEntry* entry)
{
	const long rows = GetItemCount();
	if (auto it = std::find_if(m_sorted.begin(), m_sorted.end(), [entry](const auto& r) { return &r.get().entry == entry; }); it != m_sorted.end())
		m_sorted.erase(it);
	if (auto it = std::find_if(m_data.begin(), m_data.end(), [entry](const auto& p) { return &p->entry == entry; }); it != m_data.end())
		m_data.erase(it);
	ShowRows(std::max(0L, rows - 1));
}

void wxTitleManagerList::ShowRows(size_t count)
{
	SetItemCount(count);
	RefreshPage();
}

size_t wxTitleManagerList::GetCountByType(EntryType type) const
{
	return std::count_if(m_data.begin(), m_data.end(), [type](const auto& p) { return p->entry.type == type; });
}

void wxTitleManagerList::ClearItems()
{
	m_sorted.clear();
	m_data.clear();
	ShowRows(0);
}

void wxTitleManagerList::AutosizeColumns()
{
	wxAutosizeColumns(this, ColumnTitleId, ColumnMAX - 1);
}

void wxTitleManagerList::RefreshPage()
{
	const long rows = GetItemCount();
	if (rows <= 0)
		return;
	const long top = GetTopItem();
	RefreshItems(top, std::min(rows - 1, top + GetCountPerPage() + 1));
}

// ---------------------------------------------------------------------------
// title / save list callbacks (called on the scanner threads)

void wxTitleManagerList::QueueTitleEvent(CafeTitleListCallbackEvent* evt)
{
	const bool found = evt->eventType == CafeTitleListCallbackEvent::TYPE::TITLE_DISCOVERED;
	if (!found && evt->eventType != CafeTitleListCallbackEvent::TYPE::TITLE_REMOVED)
		return;

	TitleInfo& title = *evt->titleInfo;
	TitleEntry entry(TypeOf(title), FormatOf(title), title.GetPath());
	entry.location_uid = title.GetUID();
	entry.title_id = title.GetAppTitleId();
	if (!found)
	{
		PostEntry(this, wxEVT_TITLE_REMOVED, entry);
		return;
	}
	// cached entries and system data titles are not listed
	if (title.IsCached() || title.IsSystemDataTitle())
		return;
	ParsedMetaXml* meta = title.GetMetaInfo();
	entry.name = DisplayName(meta);
	entry.version = title.GetAppTitleVersion();
	entry.region = meta->GetRegion();
	PostEntry(this, wxEVT_TITLE_FOUND, entry);
}

void wxTitleManagerList::QueueSaveEvent(CafeSaveListCallbackEvent* evt)
{
	if (evt->eventType != CafeSaveListCallbackEvent::TYPE::SAVE_DISCOVERED)
		return;
	ParsedMetaXml* meta = evt->saveInfo->GetMetaInfo();
	if (meta == nullptr)
		return;
	TitleEntry entry(EntryType::Save, EntryFormat::Folder, evt->saveInfo->GetPath());
	entry.location_uid = std::hash<uint64>()(meta->GetTitleId());
	entry.title_id = meta->GetTitleId();
	entry.name = DisplayName(meta);
	entry.version = meta->GetTitleVersion();
	entry.region = meta->GetRegion();
	PostEntry(this, wxEVT_TITLE_FOUND, entry);
}

void wxTitleManagerList::OnTitleFoundEvent(wxCommandEvent& event)
{
	auto* data = dynamic_cast<TitleEntryData_t*>(event.GetClientObject());
	wxASSERT(data);
	AddTitle(data);
}

void wxTitleManagerList::OnTitleRemovedEvent(wxCommandEvent& event)
{
	auto* data = dynamic_cast<TitleEntryData_t*>(event.GetClientObject());
	wxASSERT(data);
	if (auto known = EntryByUid(data->GetData().location_uid))
		RemoveEntry(&*known);
}

// ---------------------------------------------------------------------------
// display

wxString wxTitleManagerList::TypeName(EntryType type)
{
	switch (type)
	{
	case EntryType::Base: return _("base");
	case EntryType::Update: return _("update");
	case EntryType::Dlc: return _("DLC");
	case EntryType::Save: return _("save");
	case EntryType::System: return _("system");
	}
	return std::to_string((int)type);
}

wxString wxTitleManagerList::ColumnText(const TitleEntry& entry, ItemColumn column)
{
	switch (column)
	{
	case ColumnTitleId:
		return formatWxString("{:08x}-{:08x}", (uint32)(entry.title_id >> 32), (uint32)entry.title_id);
	case ColumnName:
		return entry.name;
	case ColumnType:
		return TypeName(entry.type);
	case ColumnVersion:
		return formatWxString("{}", entry.version);
	case ColumnRegion:
		return wxGetTranslation(fmt::format("{}", entry.region));
	case ColumnFormat:
		if (entry.type == EntryType::Save)
			return _("Save folder");
		switch (entry.format)
		{
		case EntryFormat::Folder: return _("Folder");
		case EntryFormat::WUD: return _("WUD");
		case EntryFormat::NUS: return _("NUS");
		case EntryFormat::WUA: return _("WUA");
		case EntryFormat::WUHB: return _("WUHB");
		}
		return "";
	case ColumnLocation:
	{
		// inside the MLC means installed, anything else comes from the game paths
		const std::string inMlc = _pathToUtf8(entry.path.lexically_relative(ActiveSettings::GetMlcPath()));
		return (inMlc.starts_with("usr") || inMlc.starts_with("sys")) ? _("MLC") : _("Game Paths");
	}
	default:
		UNREACHABLE;
	}
	return wxEmptyString;
}

wxString wxTitleManagerList::OnGetItemText(long item, long column) const
{
	if (item >= GetItemCount())
		return wxEmptyString;
	if (auto e = EntryAtRow(item))
		return ColumnText(*e, (ItemColumn)column);
	return wxEmptyString;
}

wxItemAttr* wxTitleManagerList::OnGetItemAttr(long item) const
{
	// alternate row shading
	const wxColour base = GetBackgroundColour();
	m_rowAttr[0] = wxItemAttr(GetTextColour(), base, GetFont());
	m_rowAttr[1] = wxItemAttr(GetTextColour(), wxHelper::CalculateAccentColour(base), GetFont());
	return &m_rowAttr[item % 2];
}

void wxTitleManagerList::SortEntries(int column)
{
	bool ascending;
	if (column != -1)
	{
		ascending = GetUpdatedAscendingSortIndicator(column); // a header click toggles the direction
	}
	else
	{
		// re-sort by the current indicator, by title id when there is none
		const int current = GetSortIndicator();
		column = current != -1 ? current : ColumnTitleId;
		ascending = IsAscendingSortIndicator();
	}

	if (column < ColumnTitleId || column > ColumnFormat)
		return; // location is not sortable

	auto nameCmp = [](const TitleEntry& a, const TitleEntry& b) { return a.name.CmpNoCase(b.name); };
	std::sort(m_sorted.begin(), m_sorted.end(), [&](const std::reference_wrapper<ItemData>& x, const std::reference_wrapper<ItemData>& y) {
		const ItemData& a = ascending ? x.get() : y.get();
		const ItemData& b = ascending ? y.get() : x.get();
		return titlesort::Before(column, a.visible, a.entry, b.visible, b.entry, nameCmp);
	});
	ShowSortIndicator(column, ascending);
	RefreshPage();
}

void wxTitleManagerList::Filter(const wxString& filter)
{
	if (filter.empty())
	{
		for (auto& item : m_data)
			item->visible = true;
		ShowRows(m_data.size());
		return;
	}

	wxString needle = filter.Upper();
	needle.Trim(false).Trim(true);

	struct Prefix
	{
		const char* text;
		ItemColumn column;
	};
	static constexpr Prefix kPrefixes[] = {
		{"TITLEID:", ColumnTitleId},
		{"NAME:", ColumnName},
		{"TYPE:", ColumnType},
		{"REGION:", ColumnRegion},
		{"VERSION:", ColumnVersion},
		{"FORMAT:", ColumnFormat},
	};

	std::function<bool(const TitleEntry&)> match;
	for (const Prefix& p : kPrefixes)
	{
		if (!needle.StartsWith(p.text))
			continue;
		wxString value = needle.Mid(strlen(p.text));
		value.Trim(false);
		const ItemColumn col = p.column;
		match = [value, col](const TitleEntry& e) { return ColumnText(e, col).Upper().Contains(value); };
		break;
	}
	if (!match)
	{
		if (needle == "ERROR")
			match = [](const TitleEntry&) { return true; };
		else
			match = [needle](const TitleEntry& e) {
				return e.name.Upper().Contains(needle) || ColumnText(e, ColumnTitleId).Upper().Contains(needle) || ColumnText(e, ColumnType).Upper().Contains(needle);
			};
	}

	size_t shown = 0;
	for (auto& item : m_data)
	{
		item->visible = match(item->entry);
		shown += item->visible ? 1 : 0;
	}
	ShowRows(shown);
}

// ---------------------------------------------------------------------------
// context menu

bool wxTitleManagerList::IsBusy()
{
	return m_context_worker.valid() && !future_is_ready(m_context_worker);
}

void wxTitleManagerList::OnContextMenu(wxContextMenuEvent& /*event*/)
{
	if (IsBusy())
		return;
	const auto entry = EntryAtRow(GetFirstSelected());
	if (!entry)
		return;

	wxMenu menu;
	if (entry->type == EntryType::Base)
		menu.Append(ID_LAUNCH, _("&Launch title"));
	menu.Append(ID_OPEN_DIRECTORY, _("&Open directory"));
	if (entry->type != EntryType::Save)
		menu.Append(ID_VERIFY, _("&Verify integrity of game files"));
	menu.AppendSeparator();
	if (entry->type != EntryType::Save && entry->format != EntryFormat::WUA)
	{
		menu.Append(ID_CONVERT_WUA, _("Convert to compressed Wii U archive (.wua)"));
		menu.AppendSeparator();
	}
	menu.Append(ID_DELETE, _("&Delete"));
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent& evt) { OnMenuCommand(evt.GetId()); });
	PopupMenu(&menu);
}

void wxTitleManagerList::OnMenuCommand(int id)
{
	if (IsBusy())
		return;
	const long row = GetFirstSelected();
	auto entry = EntryAtRow(row);
	if (!entry)
		return;

	switch (id)
	{
	case ID_OPEN_DIRECTORY:
		wxLaunchDefaultApplication(wxHelper::FromPath(fs::is_directory(entry->path) ? entry->path : entry->path.parent_path()));
		break;
	case ID_DELETE:
		DeleteWithConfirmation(row, *entry);
		break;
	case ID_LAUNCH:
		try
		{
			MainWindow::RequestLaunchGame(entry->path, wxLaunchGameEvent::INITIATED_BY::TITLE_MANAGER);
			Close();
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "wxTitleManagerList::OnContextMenuSelected: can't launch title: {}", ex.what());
		}
		break;
	case ID_VERIFY:
		(new ChecksumTool(this, *entry))->Show();
		break;
	case ID_CONVERT_WUA:
		ConvertToWua(entry->title_id, entry->location_uid);
		break;
	default:
		break;
	}
}

void wxTitleManagerList::DeleteWithConfirmation(long row, const TitleEntry& entry)
{
	// asked here on the UI thread; only the file removal runs in the background
	const bool isFolder = fs::is_directory(entry.path);
	const wxString question = isFolder
		? formatWxString(_("Are you really sure that you want to delete the following folder:\n{}"), _pathToUtf8(entry.path))
		: formatWxString(_("Are you really sure that you want to delete the following file:\n{}"), _pathToUtf8(entry.path));
	if (wxMessageBox(question, _("Warning"), wxYES_NO | wxCENTRE | wxICON_EXCLAMATION, this) == wxNO)
		return;

	const fs::path path = entry.path;
	const bool isSave = entry.type == EntryType::Save;
	m_context_worker = std::async(std::launch::async, [this, row, path, isFolder, isSave]() -> bool {
		wxQueueEvent(this, new wxSetStatusBarTextEvent("Deleting entry...", 1));
		std::error_code ec;
		if (isFolder)
		{
			// remove the title's own sub folders, then the folder itself if nothing else is left in it
			const char* const titleParts[] = {"content", "meta", "code"};
			const char* const saveParts[] = {"meta", "user"};
			for (const char* part : isSave ? std::span<const char* const>(saveParts) : std::span<const char* const>(titleParts))
				fs::remove_all(path / part, ec);
			if (fs::is_empty(path, ec))
				fs::remove_all(path, ec);
		}
		else
			fs::remove(path, ec);

		bool ok = true;
		if (ec)
		{
			const wxString message = formatWxString(_("Error when trying to delete the entry:\n{}"), GetSystemErrorMessage(ec));
			CallAfter([this, message] { wxMessageBox(message, _("Error"), wxOK | wxCENTRE, this); });
			ok = false;
		}
		else
		{
			auto* removed = new wxCommandEvent(wxEVT_REMOVE_ITEM);
			removed->SetInt(row);
			wxQueueEvent(this, removed);
		}
		wxQueueEvent(this, new wxSetStatusBarTextEvent(wxEmptyString, 1));
		return ok;
	});
}

void wxTitleManagerList::ConvertToWua(uint64 titleId, uint64 clickedUid)
{
	// a separate update selects its base game
	titleId = TitleIdParser::MakeBaseTitleId(titleId);
	const TitleIdParser parser(titleId);
	const bool hasBase = parser.GetType() != TitleIdParser::TITLE_TYPE::AOC;
	const bool hasUpdate = parser.CanHaveSeparateUpdateTitleId();
	const TitleId updateId = hasUpdate ? parser.GetSeparateUpdateTitleId() : 0;
	// assumes the DLC title id only differs in the type field
	const TitleId dlcId = hasBase ? ((titleId & ~(uint64)0xFF00000000) | (uint64)0xC00000000) : titleId;

	TitleInfo base, update, dlc;
	// base: first match, or the clicked one
	if (hasBase)
	{
		for (const auto& item : m_data)
		{
			if (item->entry.title_id != titleId || base.IsValid())
				continue;
			base = CafeTitleList::GetTitleInfoByUID(item->entry.location_uid);
			if (item->entry.location_uid == clickedUid)
				break;
		}
	}
	// update: the newest one seen, stopping at the clicked one
	if (hasUpdate)
	{
		for (const auto& item : m_data)
		{
			if (item->entry.title_id != updateId)
				continue;
			if (!update.IsValid() || update.GetAppTitleVersion() < item->entry.version)
				update = CafeTitleList::GetTitleInfoByUID(item->entry.location_uid);
			if (item->entry.location_uid == clickedUid)
				break;
		}
	}
	// DLC: the clicked one, else the last listed
	for (const auto& item : m_data)
	{
		if (item->entry.title_id != dlcId)
			continue;
		dlc = CafeTitleList::GetTitleInfoByUID(item->entry.location_uid);
		if (item->entry.location_uid == clickedUid)
			break;
	}

	wxString summary = _("The following content will be converted to a compressed Wii U archive file (.wua):");
	summary.append("\n \n");
	summary.append(base.IsValid() ? formatWxString(_("Base game:\n{}"), base.GetPrintPath()) : _("Base game:\nNot installed"));
	summary.append("\n\n");
	summary.append(update.IsValid() ? formatWxString(_("Update:\n{}"), update.GetPrintPath()) : _("Update:\nNot installed"));
	summary.append("\n\n");
	summary.append(dlc.IsValid() ? formatWxString(_("DLC:\n{}"), dlc.GetPrintPath()) : _("DLC:\nNot installed"));
	if (wxMessageBox(summary, _("Confirmation"), wxOK | wxCANCEL | wxCENTRE | wxICON_QUESTION, this) != wxOK)
		return;

	std::vector<TitleInfo*> parts;
	for (TitleInfo* t : {&base, &update, &dlc})
	{
		if (t->IsValid())
			parts.emplace_back(t);
	}
	if (parts.empty())
		return;

	// suggested file name: "<short name> (<region>) (v<update version>).wua"
	std::string fileName = parts.front()->GetMetaInfo()->GetShortName(CafeConsoleLanguage::EN);
	boost::replace_all(fileName, ":", "");
	boost::replace_all(fileName, "/", "");
	boost::replace_all(fileName, "\\", "");
	CafeConsoleRegion region = CafeConsoleRegion::Auto;
	if (base.IsValid() && base.HasValidXmlInfo())
		region = base.GetMetaInfo()->GetRegion();
	else if (update.IsValid() && update.HasValidXmlInfo())
		region = update.GetMetaInfo()->GetRegion();
	if (region == CafeConsoleRegion::JPN)
		fileName += " (JP)";
	else if (region == CafeConsoleRegion::EUR)
		fileName += " (EU)";
	else if (region == CafeConsoleRegion::USA)
		fileName += " (US)";
	if (update.IsValid())
		fileName += fmt::format(" (v{})", update.GetAppTitleVersion());
	fileName += ".wua";

	const std::string startDir = GetConfig().game_paths.empty() ? std::string() : std::string(GetConfig().game_paths.front());
	wxFileDialog saveAs(this, _("Save Wii U game archive file"), startDir, wxString::FromUTF8(fileName), "WUA files (*.wua)|*.wua", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (saveAs.ShowModal() == wxID_CANCEL || saveAs.GetPath().IsEmpty())
		return;
	const fs::path finalPath = wxHelper::MakeFSPath(saveAs.GetPath());
	const fs::path tempPath = wxHelper::MakeFSPath(saveAs.GetPath() + "__tmp");

	WuaBuilder builder(tempPath);
	if (!builder.Opened())
	{
		wxMessageBox(_("Unable to create file"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}

	wxGenericProgressDialog progress("Converting to .wua", _("Counting files..."), 100, this, wxPD_CAN_ABORT);
	progress.Show();
	auto job = std::async(std::launch::async, &WuaBuilder::Build, &builder, parts);
	while (!future_is_ready(job))
	{
		if (builder.cancel)
			progress.Update(0, _("Stopping..."));
		else if (builder.m_filesDone != 0)
		{
			const uint64 done = builder.m_bytesDone, total = builder.m_bytesTotal;
			// stay below 100: reaching the range would make Update() go modal
			const uint32 pct = total == 0 ? 99 : std::min<uint32>((uint32)(done * 100 / total), 99);
			progress.Update(pct, _("Converting files...") + fmt::format(" ({}MiB/{}MiB)", done / 1024 / 1024, total / 1024 / 1024));
		}
		else
			progress.Update(0, _("Collecting list of files...") + fmt::format(" ({})", builder.m_fileCount.load()));
		if (progress.WasCancelled())
			builder.cancel = true;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	progress.Update(99, _("Finalizing..."));

	std::error_code ec;
	if (!job.get())
	{
		builder.CloseFile();
		fs::remove(tempPath, ec);
		return;
	}
	builder.Finish();

	// make sure the archive can be opened before it replaces anything
	if (ZArchiveReader* check = ZArchiveReader::OpenFromFile(tempPath))
		delete check;
	else
	{
		wxMessageBox(_("Conversion failed\n"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		fs::remove(tempPath, ec);
		return;
	}
	progress.Hide();
	fs::rename(tempPath, finalPath);

	CafeTitleList::Refresh();
	wxMessageBox(_("Conversion finished\n"), _("Complete"), wxOK | wxCENTRE | wxICON_INFORMATION, this);
}
