// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/components/wxGameList.h"

#include "wxgui/helpers/wxCustomData.h"
#include "wxCemuConfig.h"
#include "util/helpers/helpers.h"
#include "wxgui/GameProfileWindow.h"

#include <numeric>

#include <wx/listctrl.h>
#include <wx/wupdlock.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/imaglist.h>
#include <wx/textdlg.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/wfstream.h>
#include <wx/imagpng.h>
#include <wx/string.h>
#include <wx/utils.h>
#include <wx/clipbrd.h>

#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Cafe/TitleList/GameInfo.h"
#include "Cafe/TitleList/TitleList.h"

#include "wxgui/CemuApp.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/MainWindow.h"

#include "../wxHelper.h"

#include "Cafe/IOSU/PDM/iosu_pdm.h" // play time and last played date

#if BOOST_OS_WINDOWS
#include <windows.h>
#include <winnls.h>
#include <shobjidl.h>
#include <objbase.h>
#include <objidl.h>
#include <shlguid.h>
#include <shlobj.h>
#include <wrl/client.h>
#endif

wxDEFINE_EVENT(wxEVT_OPEN_SETTINGS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_GAMELIST_BEGIN_UPDATE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_GAMELIST_END_UPDATE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_OPEN_GRAPHIC_PACK, wxTitleIdEvent);

// ==== gamelist-sort begin ====
// Row ordering, independent of wx so it can be tested on its own. The source
// supplies Favorite(id), Name(id), LastPlayed(id), Minutes(id) and Region(id).
namespace gamelistsort
{
	enum class Key
	{
		Name, // favourites first, then by name
		LastPlayed,
		PlayTime,
		Region,
		TitleId,
	};

	template<typename TSource>
	std::weak_ordering Compare(Key key, uint64 a, uint64 b, TSource& src)
	{
		switch (key)
		{
		case Key::LastPlayed:
			return src.LastPlayed(a) <=> src.LastPlayed(b);
		case Key::PlayTime:
			return src.Minutes(a) <=> src.Minutes(b);
		case Key::Region:
			return src.Region(a) <=> src.Region(b);
		case Key::TitleId:
			return a <=> b;
		case Key::Name:
		default:
		{
			const bool favA = src.Favorite(a), favB = src.Favorite(b);
			if (favA != favB)
				return favA ? std::weak_ordering::less : std::weak_ordering::greater;
			const int c = src.Name(a).compare(src.Name(b));
			return c < 0 ? std::weak_ordering::less : (c > 0 ? std::weak_ordering::greater : std::weak_ordering::equivalent);
		}
		}
	}

	// -1, 0 or 1, flipped when descending
	inline int Signed(std::weak_ordering o, bool ascending)
	{
		const int v = o < 0 ? -1 : (o > 0 ? 1 : 0);
		return ascending ? v : -v;
	}

	// first index whose row does not sort before the new one; cmpAt(i) compares the new row with row i
	template<typename TCmp>
	long LowerBound(long count, TCmp cmpAt)
	{
		long lo = 0, hi = count;
		while (lo < hi)
		{
			const long mid = lo + (hi - lo) / 2;
			if (cmpAt(mid) > 0)
				lo = mid + 1;
			else
				hi = mid;
		}
		return lo;
	}
}
// ==== gamelist-sort end ====

namespace
{
	wxDEFINE_EVENT(wxEVT_GAME_ENTRY_ADDED_OR_REMOVED, wxTitleIdEvent);

	// row data of the "open settings" placeholder row
	constexpr uint64 kSettingsRowData = 0x1337;

	enum
	{
		ID_GAME_START = wxID_HIGHEST + 1,
		ID_GAME_FAVORITE,
		ID_GAME_RENAME,
		ID_GAME_DIR,
		ID_GAME_SAVE_DIR,
		ID_GAME_UPDATE_DIR,
		ID_GAME_DLC_DIR,
		ID_GAME_GRAPHIC_PACKS,
		ID_GAME_PROFILE,
		ID_GAME_REMOVE_CACHES,
		ID_GAME_SHORTCUT,
		ID_GAME_COPY_NAME,
		ID_GAME_COPY_ID,
		ID_GAME_COPY_IMAGE,
		ID_LIST_REFRESH,
		ID_LIST_STYLE_LIST,
		ID_LIST_STYLE_ICONS,
		ID_LIST_STYLE_SMALL_ICONS,
	};

	enum
	{
		ID_COL_RESET_WIDTH = wxID_HIGHEST + 1,
		ID_COL_RESET_ORDER,
		ID_COL_SHOW_ICON,
		ID_COL_SHOW_NAME,
		ID_COL_SHOW_VERSION,
		ID_COL_SHOW_DLC,
		ID_COL_SHOW_PLAY_TIME,
		ID_COL_SHOW_LAST_PLAYED,
		ID_COL_SHOW_REGION,
		ID_COL_SHOW_TITLE_ID,
	};

	// folder of a title; titles stored as a single file open their containing folder
	fs::path FolderOf(fs::path p)
	{
		if (p.has_extension())
			p = p.parent_path();
		return p;
	}

	void OpenFolder(const fs::path& p)
	{
		wxLaunchDefaultApplication(wxHelper::FromPath(p));
	}

	// the shader and pipeline cache files of a title that exist on disk
	std::vector<fs::path> ExistingShaderCaches(TitleId titleId)
	{
		static constexpr const wchar_t* kPatterns[] = {
			L"shaderCache/driver/vk/{:016x}.bin",
			L"shaderCache/precompiled/{:016x}_spirv.bin",
			L"shaderCache/precompiled/{:016x}_gl.bin",
			L"shaderCache/precompiled/{:016x}_air.bin",
			L"shaderCache/transferable/{:016x}_shaders.bin",
			L"shaderCache/transferable/{:016x}_mtlshaders.bin",
			L"shaderCache/transferable/{:016x}_vkpipeline.bin",
			L"shaderCache/transferable/{:016x}_mtlpipeline.bin",
		};
		std::vector<fs::path> found;
		for (const wchar_t* pattern : kPatterns)
		{
			fs::path p = ActiveSettings::GetCachePath(pattern, titleId);
			std::error_code ec;
			if (fs::exists(p, ec))
				found.emplace_back(std::move(p));
		}
		return found;
	}

	bool CopyToClipboard(wxDataObject* data)
	{
		wxClipboard* cb = wxClipboard::Get();
		if (!cb->Open())
		{
			delete data;
			return false;
		}
		cb->SetData(data);
		cb->Close();
		return true;
	}

	wxString PlayTimeText(uint32 minutes)
	{
		if (minutes == 0)
			return wxEmptyString;
		if (minutes < 60)
			return formatWxString(wxPLURAL("{} minute", "{} minutes", minutes), minutes);
		const uint32 h = minutes / 60, m = minutes % 60;
		return formatWxString(wxPLURAL("{} hour", "{} hours", h), h) + " " + formatWxString(wxPLURAL("{} minute", "{} minutes", m), m);
	}

	wxString LastPlayedText(const iosu::pdm::GameListStat::LastPlayDate& date)
	{
		if (date.year == 0)
			return _("never");
		const wxDateTime when((wxDateTime::wxDateTime_t)date.day, (wxDateTime::Month)date.month, (wxDateTime::wxDateTime_t)date.year, 0, 0, 0, 0);
		return when.FormatDate();
	}

	iosu::pdm::GameListStat PlayStat(uint64 titleId)
	{
		iosu::pdm::GameListStat stat{};
		if (!iosu::pdm::GetStatForGamelist(titleId, stat))
			stat = {};
		return stat;
	}

	// writes a single-image .icns (type ic07, a 128x128 PNG)
	[[maybe_unused]] bool WritePngAsIcns(const fs::path& png, const fs::path& icns)
	{
		std::ifstream in(png, std::ios::binary);
		if (!in)
			return false;
		const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::ofstream out(icns, std::ios::binary);
		if (!out)
			return false;
		auto putBE32 = [&out](uint32 v) {
			const char b[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
			out.write(b, 4);
		};
		const uint32 pngSize = (uint32)data.size();
		out.write("icns", 4);
		putBE32(16 + pngSize); // file header + entry header + data
		out.write("ic07", 4);
		putBE32(pngSize);
		out.write(data.data(), data.size());
		return true;
	}

	// saves the large icon of a title as PNG; empty when the icon is not loaded or cannot be written
	[[maybe_unused]] std::optional<fs::path> SaveIconPng(wxImageList& icons, int index, const fs::path& folder, TitleId titleId)
	{
		if (!fs::exists(folder) && !fs::create_directories(folder))
		{
			cemuLog_log(LogType::Force, "Failed to create icon directory");
			return std::nullopt;
		}
		fs::path out = folder / fmt::format("{:016x}.png", titleId);
		wxFileOutputStream stream(_pathToUtf8(out));
		wxImage image = wxBitmap(icons.GetIcon(index)).ConvertToImage();
		wxPNGHandler png;
		if (!png.SaveFile(&image, stream, false))
		{
			cemuLog_log(LogType::Force, "Icon failed to save");
			return std::nullopt;
		}
		return out;
	}
}

// ---------------------------------------------------------------------------
// construction

wxGameList::wxGameList(wxWindow* parent, wxWindowID id)
	: wxListView(parent, id, wxDefaultPosition, wxDefaultSize, StyleFlags(Style::kList))
{
	const auto& gui = GetWxGUIConfig();

	// image 0 of both lists is a blank placeholder for titles whose icon is not loaded
	std::vector<char> bits(kIconWidth * kIconWidth * 4, wxSystemSettings::GetAppearance().IsDark() ? (char)0xFF : (char)0x00);
	wxBitmap blank(bits.data(), kIconWidth, kIconWidth);
	m_largeIcons.Add(blank);
	wxListCtrl::SetImageList(&m_largeIcons, wxIMAGE_LIST_NORMAL);
	wxBitmap::Rescale(blank, {kListIconWidth, kListIconWidth});
	m_smallIcons.Add(blank);
	wxListCtrl::SetImageList(&m_smallIcons, wxIMAGE_LIST_SMALL);

	InsertColumn(kColHidden, "", wxLIST_FORMAT_LEFT, 0);
	InsertColumn(kColIcon, _("Icon"), wxLIST_FORMAT_CENTER, gui.show_icon_column ? DefaultColumnWidth(kColIcon) : 0);
	InsertColumn(kColName, _("Game"), wxLIST_FORMAT_LEFT, gui.column_width.name);
	InsertColumn(kColVersion, _("Version"), wxLIST_FORMAT_LEFT, gui.column_width.version);
	InsertColumn(kColDlc, _("DLC"), wxLIST_FORMAT_LEFT, gui.column_width.dlc);
	InsertColumn(kColPlayTime, _("You've played"), wxLIST_FORMAT_LEFT, gui.column_width.game_time);
	InsertColumn(kColLastPlayed, _("Last played"), wxLIST_FORMAT_LEFT, gui.column_width.game_started);
	InsertColumn(kColRegion, _("Region"), wxLIST_FORMAT_LEFT, gui.column_width.region);
	InsertColumn(kColTitleId, _("Title ID"), wxLIST_FORMAT_LEFT, gui.column_width.title_id);

	Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& event) {
		event.Skip();
		CallAfter([this] {
			RecolourRows();
			Refresh();
		});
	});
	Bind(wxEVT_CLOSE_WINDOW, &wxGameList::OnClose, this);
	// mouse motion and leave events are consumed here (not passed on to the native control)
	Bind(wxEVT_MOTION, [](wxMouseEvent&) {});
	Bind(wxEVT_LEAVE_WINDOW, [](wxMouseEvent&) {});
	Bind(wxEVT_LIST_KEY_DOWN, &wxGameList::OnKeyDown, this);
	Bind(wxEVT_CONTEXT_MENU, &wxGameList::OnContextMenu, this);
	Bind(wxEVT_LIST_ITEM_ACTIVATED, &wxGameList::OnItemActivated, this);
	Bind(wxEVT_GAME_ENTRY_ADDED_OR_REMOVED, &wxGameList::OnTitleListChanged, this);
	Bind(wxEVT_LIST_COL_CLICK, &wxGameList::OnColumnClick, this);
	Bind(wxEVT_LIST_COL_BEGIN_DRAG, &wxGameList::OnColumnBeginResize, this);
	Bind(wxEVT_LIST_COL_END_DRAG, &wxGameList::OnColumnResize, this);
	Bind(wxEVT_LIST_COL_RIGHT_CLICK, &wxGameList::OnColumnRightClick, this);
	Bind(wxEVT_SIZE, &wxGameList::OnSize, this);
	m_batchTimer.Bind(wxEVT_TIMER, &wxGameList::FlushPendingTitles, this);

	RegisterTitleListCallback();

	m_iconThreadRun = true;
	m_iconThread = std::thread(&wxGameList::IconWorker, this);

	ShowSortIndicator(kColName);
}

wxGameList::~wxGameList()
{
	CafeTitleList::UnregisterCallback(m_callbackIdTitleList);
	m_batchTimer.Stop();

	m_iconThreadRun = false;
	m_iconRequests.increment(); // wake the worker so it sees the stop flag
	m_iconThread.join();
}

void wxGameList::RegisterTitleListCallback()
{
	// registering replays a discovery event for every title already known
	m_callbackIdTitleList = CafeTitleList::RegisterCallback(
		[](CafeTitleListCallbackEvent* evt, void* ctx) {
			if (evt->eventType != CafeTitleListCallbackEvent::TYPE::TITLE_DISCOVERED && evt->eventType != CafeTitleListCallbackEvent::TYPE::TITLE_REMOVED)
				return;
			auto* list = static_cast<wxGameList*>(ctx);
			wxQueueEvent(list, new wxTitleIdEvent(wxEVT_GAME_ENTRY_ADDED_OR_REMOVED, evt->titleInfo->GetAppTitleId()));
		},
		this);
}

long wxGameList::StyleFlags(Style style)
{
	switch (style)
	{
	case Style::kList:
		return wxLC_SINGLE_SEL | wxLC_VRULES | wxLC_REPORT;
	case Style::kIcons:
	case Style::kSmallIcons:
		return wxLC_SINGLE_SEL | wxLC_ICON | wxLC_ALIGN_TOP | wxLC_AUTOARRANGE;
	}
	wxASSERT(false);
	return wxLC_SINGLE_SEL | wxLC_REPORT;
}

int wxGameList::DefaultColumnWidth(int column)
{
	switch (column)
	{
	case kColIcon: return kListIconWidth + 2;
	case kColName: return DefaultColumnSize::name;
	case kColVersion: return DefaultColumnSize::version;
	case kColDlc: return DefaultColumnSize::dlc;
	case kColPlayTime: return DefaultColumnSize::game_time;
	case kColLastPlayed: return DefaultColumnSize::game_started;
	case kColRegion: return DefaultColumnSize::region;
	case kColTitleId: return DefaultColumnSize::title_id;
	default: return 80;
	}
}

// ---------------------------------------------------------------------------
// configuration and style

void wxGameList::LoadConfig()
{
	const auto& gui = GetWxGUIConfig();
	SetStyle((Style)gui.game_list_style, false);

	if (gui.game_list_column_order.empty())
		return;
	// stored as "[a, b, c, ...]"
	wxArrayInt order;
	std::string_view text(gui.game_list_column_order);
	text.remove_prefix(1);
	for (std::string_view part : TokenizeView(text, ','))
	{
		if (!part.empty())
			order.push_back(ConvertString<int>(std::string(part), 10));
	}
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
	if (order.GetCount() == kColCount)
		SetColumnsOrder(order);
#endif
}

void wxGameList::SaveConfig(bool flush)
{
	auto& gui = GetWxGUIConfig();
	gui.game_list_style = (int)m_style;
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
	gui.game_list_column_order = fmt::format("{}", GetColumnsOrder());
#endif
	if (flush)
		GetConfigHandle().Save();
}

void wxGameList::SetStyle(Style style, bool save)
{
	if (style == m_style)
		return;

	wxWindowUpdateLocker freeze(this);
	// keep the selected title selected once the rows are back
	if (const long sel = GetFirstSelected(); sel != wxNOT_FOUND)
		m_pendingSelection = (TitleId)GetItemData(sel);

	m_style = style;
	SetWindowStyleFlag(StyleFlags(style));
	if (style == Style::kIcons)
		wxListCtrl::SetImageList(&m_largeIcons, wxIMAGE_LIST_NORMAL);
	else if (style == Style::kSmallIcons)
		wxListCtrl::SetImageList(&m_smallIcons, wxIMAGE_LIST_NORMAL);
	else
		wxListCtrl::SetImageList(&m_smallIcons, wxIMAGE_LIST_SMALL);

	ReloadGameEntries();
	SortRows();
	RecolourRows();

	if (save)
	{
		GetWxGUIConfig().game_list_style = (int)m_style;
		GetConfigHandle().Save();
	}
	if (style == Style::kList)
		ApplyGameListColumnWidths();
}

bool wxGameList::IsVisible(long item) const
{
	wxRect row;
	GetItemRect(item, row);
	return GetClientRect().Intersects(row);
}

void wxGameList::OnClose(wxCloseEvent& event)
{
	event.Skip();
	m_exit = true;
}

// ---------------------------------------------------------------------------
// rows

void wxGameList::ReloadGameEntries()
{
	wxWindowUpdateLocker freeze(this);
	DeleteAllItems();
	CafeTitleList::Refresh();
	CafeTitleList::UnregisterCallback(m_callbackIdTitleList);
	RegisterTitleListCallback();
}

void wxGameList::DeleteCachedStrings()
{
	m_nameCache.clear();
}

long wxGameList::FindListItemByTitleId(uint64 title_id) const
{
	const long rows = GetItemCount();
	for (long i = 0; i < rows; i++)
	{
		if ((uint64)GetItemData(i) == title_id)
			return i;
	}
	return wxNOT_FOUND;
}

std::string wxGameList::TitleName(uint64 titleId)
{
	if (auto it = m_nameCache.find(titleId); it != m_nameCache.end())
		return it->second;
	TitleInfo info;
	if (!CafeTitleList::GetFirstByTitleId(titleId, info))
		return "Unknown title";
	std::string name;
	if (!GetConfig().GetGameListCustomName(titleId, name))
		name = info.GetMetaTitleName();
	m_nameCache.emplace(titleId, name);
	return name;
}

std::weak_ordering wxGameList::CompareTitles(int column, uint64 a, uint64 b)
{
	struct Source
	{
		wxGameList& list;
		bool Favorite(uint64 id) { return GetConfig().IsGameListFavorite(id); }
		std::string Name(uint64 id) { return list.TitleName(id); }
		iosu::pdm::GameListStat::LastPlayDate LastPlayed(uint64 id) { return PlayStat(id).last_played; }
		uint32 Minutes(uint64 id) { return PlayStat(id).numMinutesPlayed; }
		CafeConsoleRegion Region(uint64 id) { return CafeTitleList::GetGameInfo(id).GetRegion(); }
	} source{*this};

	using gamelistsort::Key;
	Key key = Key::Name;
	switch (column)
	{
	case kColLastPlayed: key = Key::LastPlayed; break;
	case kColPlayTime: key = Key::PlayTime; break;
	case kColRegion: key = Key::Region; break;
	case kColTitleId: key = Key::TitleId; break;
	default: break;
	}
	return gamelistsort::Compare(key, a, b, source);
}

void wxGameList::SortRows(int column)
{
	bool ascending;
	if (column < 0)
	{
		column = GetSortIndicator();
		if (column < 0)
			column = kColName;
		ascending = IsAscendingSortIndicator();
	}
	else
		ascending = GetUpdatedAscendingSortIndicator(column);

	// only these columns can be sorted by
	if (column != kColName && column != kColPlayTime && column != kColLastPlayed && column != kColRegion && column != kColTitleId)
		return;

	struct Context
	{
		wxGameList* list;
		int column;
		bool ascending;
	} ctx{this, column, ascending};
	SortItems([](wxIntPtr a, wxIntPtr b, wxIntPtr data) -> int {
		auto* c = (Context*)data;
		return gamelistsort::Signed(c->list->CompareTitles(c->column, (uint64)a, (uint64)b), c->ascending);
	}, (wxIntPtr)&ctx);
	ShowSortIndicator(column, ascending);
}

long wxGameList::InsertPosition(TitleId titleId)
{
	const int column = GetSortIndicator();
	const bool ascending = IsAscendingSortIndicator();
	return gamelistsort::LowerBound(GetItemCount(), [&](long row) {
		return gamelistsort::Signed(CompareTitles(column, titleId, (uint64)GetItemData(row)), ascending);
	});
}

void wxGameList::RecolourRows()
{
	wxWindowUpdateLocker freeze(this);
	const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	const wxColour plain = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
	const wxColour striped = wxHelper::CalculateAccentColour(plain);
	const wxColour favourite = wxSystemSettings::SelectLightDark(wxColour(253, 246, 211), wxColour(82, 84, 48));
	const long rows = GetItemCount();
	for (long i = 0; i < rows; i++)
	{
		const uint64 titleId = GetItemData(i);
		if (GetConfig().IsGameListFavorite(titleId))
			SetItemBackgroundColour(i, favourite);
		else
			SetItemBackgroundColour(i, (i % 2) != 0 ? plain : striped);
		SetItemTextColour(i, text);
	}
}

void wxGameList::OnTitleListChanged(wxTitleIdEvent& event)
{
	// collect changes for 100ms; once 100 are queued the timer is no longer pushed back
	if (m_pendingTitles.size() < 100)
		m_batchTimer.StartOnce(100);
	m_pendingTitles.emplace_back(event.GetTitleId());
}

void wxGameList::FillRow(long row, GameInfo2& game, TitleId baseId, bool newRow, int icon, int iconSmall)
{
	if (m_style == Style::kIcons)
	{
		SetItemImage(row, icon);
		return;
	}
	if (m_style == Style::kSmallIcons)
	{
		SetItemImage(row, iconSmall);
		return;
	}

	SetItemColumnImage(row, kColIcon, iconSmall);
	SetItem(row, kColName, wxString::FromUTF8(TitleName(baseId)));
	SetItem(row, kColVersion, fmt::format("{}", game.GetVersion()));
	SetItem(row, kColDlc, game.HasAOC() ? wxString(fmt::format("{}", game.GetAOCVersion())) : wxString());
	if (newRow)
	{
		iosu::pdm::GameListStat stat{};
		if (iosu::pdm::GetStatForGamelist(baseId, stat))
		{
			SetItem(row, kColPlayTime, PlayTimeText(stat.numMinutesPlayed));
			SetItem(row, kColLastPlayed, LastPlayedText(stat.last_played));
		}
		else
		{
			SetItem(row, kColPlayTime, wxEmptyString);
			SetItem(row, kColLastPlayed, _("never"));
		}
	}
	SetItem(row, kColRegion, wxGetTranslation(fmt::format("{}", game.GetRegion())));
	SetItem(row, kColTitleId, fmt::format("{:016x}", baseId));
}

void wxGameList::FlushPendingTitles(wxTimerEvent& /*event*/)
{
	std::vector<TitleId> batch;
	batch.swap(m_pendingTitles);

	wxWindowUpdateLocker freeze(this);
	TakeDecodedIcons();
	bool added = false;
	for (TitleId reported : batch)
	{
		GameInfo2 game = CafeTitleList::GetGameInfo(reported);
		// removals are handled by full refreshes, so titles that are gone are just skipped
		if (!game.IsValid() || game.IsSystemDataTitle())
			continue;
		const TitleId baseId = game.GetBaseTitleId();

		int icon = 0, iconSmall = 0;
		QueryIconForTitle(baseId, icon, iconSmall);

		long row = FindListItemByTitleId(baseId);
		const bool newRow = row == wxNOT_FOUND;
		if (newRow)
		{
			const int image = m_style == Style::kList ? -1 : (m_style == Style::kIcons ? icon : iconSmall);
			row = InsertItem(InsertPosition(baseId), wxString::FromUTF8(TitleName(baseId)), image);
			SetItemPtrData(row, baseId);
			added = true;
		}
		FillRow(row, game, baseId, newRow, icon, iconSmall);
	}
	if (added)
		RecolourRows();

	if (m_pendingSelection)
	{
		const long row = FindListItemByTitleId(*m_pendingSelection);
		if (row != wxNOT_FOUND)
		{
			Select(row);
			Focus(row);
			EnsureVisible(row);
			m_pendingSelection.reset();
		}
	}
}

// ---------------------------------------------------------------------------
// icons

void wxGameList::IconWorker()
{
	SetThreadName("GameListWorker");
	while (m_iconThreadRun)
	{
		m_iconRequests.decrementWithWait();
		TitleId titleId;
		{
			std::lock_guard lock(m_iconMutex);
			if (m_iconQueue.empty())
				continue;
			titleId = m_iconQueue.front();
			m_iconQueue.pop_front();
		}
		// one attempt per title
		if (!m_iconAttempted.insert(titleId).second)
			continue;

		TitleInfo info;
		if (!CafeTitleList::GetFirstByTitleId(titleId, info))
			continue;
		const std::string mountPath = TitleInfo::GetUniqueTempMountingPath();
		if (!info.Mount(mountPath, "", FSC_PRIORITY_BASE))
			continue;
		auto tga = fsc_extractFile((mountPath + "/meta/iconTex.tga").c_str());
		if (!tga)
		{
			// some dumps store the icon compressed
			if (auto packed = fsc_extractFile((mountPath + "/meta/iconTex.tga.gz").c_str()))
				tga = zlibDecompress(*packed, 70 * 1024);
		}
		info.Unmount(mountPath);

		if (!tga || tga->size() <= 16)
		{
			cemuLog_log(LogType::Force, "Failed to load icon for title {:016x}", titleId);
			continue;
		}
		wxMemoryInputStream stream(tga->data(), tga->size());
		const wxImage image(stream);
		DecodedIcon decoded{titleId, image.Scale(kIconWidth, kIconWidth, wxIMAGE_QUALITY_BICUBIC), image.Scale(kListIconWidth, kListIconWidth, wxIMAGE_QUALITY_BICUBIC)};
		{
			std::lock_guard lock(m_iconMutex);
			m_decodedIcons.emplace_back(std::move(decoded));
		}
		// the row is refreshed like any other title change
		wxQueueEvent(this, new wxTitleIdEvent(wxEVT_GAME_ENTRY_ADDED_OR_REMOVED, titleId));
	}
}

void wxGameList::TakeDecodedIcons()
{
	// image lists are only touched on the UI thread
	std::vector<DecodedIcon> ready;
	{
		std::lock_guard lock(m_iconMutex);
		ready.swap(m_decodedIcons);
	}
	for (DecodedIcon& d : ready)
	{
		if (m_iconIndex.count(d.titleId) != 0)
			continue;
		const int large = m_largeIcons.Add(wxBitmap(d.largeImage));
		const int little = m_smallIcons.Add(wxBitmap(d.smallImage));
		m_iconIndex.emplace(d.titleId, std::make_pair(large, little));
	}
}

// image list indices of a title's icon; if it is not loaded yet a load is queued and false returned
bool wxGameList::QueryIconForTitle(TitleId titleId, int& icon, int& iconSmall)
{
	TakeDecodedIcons();
	const auto it = m_iconIndex.find(titleId);
	if (it != m_iconIndex.end())
	{
		icon = it->second.first;
		iconSmall = it->second.second;
		return true;
	}
	{
		std::lock_guard lock(m_iconMutex);
		m_iconQueue.push_back(titleId);
	}
	m_iconRequests.increment();
	return false;
}

// ---------------------------------------------------------------------------
// columns

int wxGameList::LastVisibleColumn() const
{
	for (int pos = GetColumnCount() - 1; pos > 0; pos--)
	{
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
		const int col = GetColumnIndexFromOrder(pos);
#else
		const int col = pos;
#endif
		if (GetColumnWidth(col) > 0)
			return col;
	}
	return 0;
}

void wxGameList::OnSize(wxSizeEvent& event)
{
	event.Skip();
	// resizing a column inside our own size event upsets sizer layouts; do it from a posted event
	wxListEvent resized(wxEVT_LIST_COL_END_DRAG);
	resized.SetColumn(LastVisibleColumn());
	wxPostEvent(this, resized);
}

void wxGameList::AdjustLastColumnWidth()
{
	// the last visible column takes the remaining width, but never less than its default
	wxWindowUpdateLocker freeze(this);
	int last = 0;
	int remaining = GetClientSize().GetWidth();
	for (int pos = 1; pos < GetColumnCount(); pos++)
	{
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
		const int col = GetColumnIndexFromOrder(pos);
#else
		const int col = pos;
#endif
		const int w = GetColumnWidth(col);
		if (w > 0)
		{
			last = col;
			remaining -= w;
		}
	}
	remaining += GetColumnWidth(last);
	SetColumnWidth(last, std::max(remaining, DefaultColumnWidth(last)));
}

void wxGameList::ApplyGameListColumnWidths()
{
	const auto& gui = GetWxGUIConfig();
	wxWindowUpdateLocker freeze(this);
	SetColumnWidth(kColIcon, gui.show_icon_column ? DefaultColumnWidth(kColIcon) : 0);
	SetColumnWidth(kColName, gui.column_width.name);
	SetColumnWidth(kColVersion, gui.column_width.version);
	SetColumnWidth(kColDlc, gui.column_width.dlc);
	SetColumnWidth(kColPlayTime, gui.column_width.game_time);
	SetColumnWidth(kColLastPlayed, gui.column_width.game_started);
	SetColumnWidth(kColRegion, gui.column_width.region);
	SetColumnWidth(kColTitleId, gui.column_width.title_id);
	AdjustLastColumnWidth();
}

void wxGameList::OnColumnClick(wxListEvent& event)
{
	SortRows(event.GetColumn());
	RecolourRows();
	event.Skip();
}

void wxGameList::OnColumnBeginResize(wxListEvent& event)
{
	// hidden columns, the icon column and the stretching last column keep their width
	const int col = event.GetColumn();
	if (GetColumnWidth(col) == 0 || col == kColIcon || col == LastVisibleColumn())
		event.Veto();
	else
		event.Skip();
}

void wxGameList::OnColumnResize(wxListEvent& event)
{
	event.Skip();
	if (m_style != Style::kList)
		return;

	auto& widths = GetWxGUIConfig().column_width;
	const int width = GetColumnWidth(event.GetColumn());
	switch (event.GetColumn())
	{
	case kColName: widths.name = width; break;
	case kColVersion: widths.version = width; break;
	case kColDlc: widths.dlc = width; break;
	case kColPlayTime: widths.game_time = width; break;
	case kColLastPlayed: widths.game_started = width; break;
	case kColRegion: widths.region = width; break;
	case kColTitleId: widths.title_id = width; break;
	default: break;
	}
	GetConfigHandle().Save();
	AdjustLastColumnWidth();
}

void wxGameList::OnColumnRightClick(wxListEvent& event)
{
	const int clicked = event.GetColumn();
	wxMenu menu;
	menu.Append(ID_COL_RESET_WIDTH, _("Reset &width"));
	menu.Append(ID_COL_RESET_ORDER, _("Reset &order"));
	menu.AppendSeparator();
	menu.AppendCheckItem(ID_COL_SHOW_ICON, _("Show &icon"))->Check(GetColumnWidth(kColIcon) > 0);
	menu.AppendCheckItem(ID_COL_SHOW_NAME, _("Show &name"))->Check(GetColumnWidth(kColName) > 0);
	menu.AppendCheckItem(ID_COL_SHOW_VERSION, _("Show &version"))->Check(GetColumnWidth(kColVersion) > 0);
	menu.AppendCheckItem(ID_COL_SHOW_DLC, _("Show &dlc"))->Check(GetColumnWidth(kColDlc) > 0);
	menu.AppendCheckItem(ID_COL_SHOW_PLAY_TIME, _("Show &game time"))->Check(GetColumnWidth(kColPlayTime) > 0);
	menu.AppendCheckItem(ID_COL_SHOW_LAST_PLAYED, _("Show &last played"))->Check(GetColumnWidth(kColLastPlayed) > 0);
	menu.AppendCheckItem(ID_COL_SHOW_REGION, _("Show &region"))->Check(GetColumnWidth(kColRegion) > 0);
	menu.AppendCheckItem(ID_COL_SHOW_TITLE_ID, _("Show &title ID"))->Check(GetColumnWidth(kColTitleId) > 0);

	menu.Bind(wxEVT_MENU, [this, clicked, &menu](wxCommandEvent& evt) {
		evt.Skip();
		auto& gui = GetWxGUIConfig();
		auto& w = gui.column_width;
		// a column toggled on comes back at its default width
		auto toggle = [&menu](int id, uint32& width, uint32 def) { width = menu.IsChecked(id) ? def : 0; };
		switch (evt.GetId())
		{
		case ID_COL_SHOW_ICON: gui.show_icon_column = menu.IsChecked(ID_COL_SHOW_ICON); break;
		case ID_COL_SHOW_NAME: toggle(ID_COL_SHOW_NAME, w.name, DefaultColumnSize::name); break;
		case ID_COL_SHOW_VERSION: toggle(ID_COL_SHOW_VERSION, w.version, DefaultColumnSize::version); break;
		case ID_COL_SHOW_DLC: toggle(ID_COL_SHOW_DLC, w.dlc, DefaultColumnSize::dlc); break;
		case ID_COL_SHOW_PLAY_TIME: toggle(ID_COL_SHOW_PLAY_TIME, w.game_time, DefaultColumnSize::game_time); break;
		case ID_COL_SHOW_LAST_PLAYED: toggle(ID_COL_SHOW_LAST_PLAYED, w.game_started, DefaultColumnSize::game_started); break;
		case ID_COL_SHOW_REGION: toggle(ID_COL_SHOW_REGION, w.region, DefaultColumnSize::region); break;
		case ID_COL_SHOW_TITLE_ID: toggle(ID_COL_SHOW_TITLE_ID, w.title_id, DefaultColumnSize::title_id); break;
		case ID_COL_RESET_WIDTH:
			switch (clicked)
			{
			case kColIcon: break; // fixed width
			case kColName: w.name = DefaultColumnSize::name; break;
			case kColVersion: w.version = DefaultColumnSize::version; break;
			case kColDlc: w.dlc = DefaultColumnSize::dlc; break;
			case kColPlayTime: w.game_time = DefaultColumnSize::game_time; break;
			case kColLastPlayed: w.game_started = DefaultColumnSize::game_started; break;
			case kColRegion: w.region = DefaultColumnSize::region; break;
			case kColTitleId: w.title_id = DefaultColumnSize::title_id; break;
			default: return;
			}
			break;
		case ID_COL_RESET_ORDER:
		{
			gui.game_list_column_order.clear();
			wxArrayInt order(kColCount);
			std::iota(order.begin(), order.end(), 0);
#ifdef wxHAS_LISTCTRL_COLUMN_ORDER
			SetColumnsOrder(order);
#endif
			break;
		}
		default:
			break;
		}
		GetConfigHandle().Save();
		ApplyGameListColumnWidths();
	});

	PopupMenu(&menu);
	event.Skip();
}

// ---------------------------------------------------------------------------
// interaction

void wxGameList::OnKeyDown(wxListEvent& event)
{
	event.Skip();
	if (m_style != Style::kList)
		return;
	// left / right page through the list
	const int key = event.GetKeyCode();
	const int rows = GetItemCount();
	if ((key != WXK_LEFT && key != WXK_RIGHT) || rows <= 0)
		return;
	const long sel = GetFirstSelected();
	int target;
	if (key == WXK_LEFT)
		target = sel == wxNOT_FOUND ? 0 : std::max(0, (int)sel - GetCountPerPage());
	else
		target = std::min(rows - 1, (sel == wxNOT_FOUND ? 0 : (int)sel) + GetCountPerPage());
	Select(target);
	Focus(target);
}

void wxGameList::OnItemActivated(wxListEvent& event)
{
	event.Skip();
	const long row = event.GetIndex();
	if (row == wxNOT_FOUND)
		return;
	const uint64 data = (uint64)GetItemData(row);
	if (data == kSettingsRowData)
	{
		wxPostEvent(this, wxCommandEvent(wxEVT_OPEN_SETTINGS));
		return;
	}
	TitleInfo info;
	if (CafeTitleList::GetFirstByTitleId(data, info) && info.IsValid())
		MainWindow::RequestLaunchGame(info.GetPath(), wxLaunchGameEvent::INITIATED_BY::GAME_LIST);
}

void wxGameList::OnContextMenu(wxContextMenuEvent& /*event*/)
{
	wxMenu menu;
	uint64 titleId = 0;

	if (const long sel = GetFirstSelected(); sel != wxNOT_FOUND)
	{
		const uint64 selectedId = (uint64)GetItemData(sel);
		GameInfo2 game = CafeTitleList::GetGameInfo(selectedId);
		if (game.IsValid())
		{
			titleId = selectedId;
			std::error_code ec;
			menu.Append(ID_GAME_START, _("&Start"));
			menu.AppendSeparator();
			menu.AppendCheckItem(ID_GAME_FAVORITE, _("&Favorite"))->Check(GetConfig().IsGameListFavorite(titleId));
			menu.Append(ID_GAME_RENAME, _("&Edit name"));
			menu.AppendSeparator();
			menu.Append(ID_GAME_DIR, _("&Game directory"));
			menu.Append(ID_GAME_SAVE_DIR, _("&Save directory"))->Enable(fs::is_directory(game.GetSaveFolder(), ec));
			menu.Append(ID_GAME_UPDATE_DIR, _("&Update directory"))->Enable(game.HasUpdate());
			menu.Append(ID_GAME_DLC_DIR, _("&DLC directory"))->Enable(game.HasAOC());
			menu.AppendSeparator();
			menu.Append(ID_GAME_REMOVE_CACHES, _("&Remove shader caches"))->Enable(!ExistingShaderCaches(game.GetBaseTitleId()).empty());
			menu.AppendSeparator();
			menu.Append(ID_GAME_GRAPHIC_PACKS, _("&Edit graphic packs"));
			menu.Append(ID_GAME_PROFILE, _("&Edit game profile"));
			menu.AppendSeparator();
			menu.Append(ID_GAME_SHORTCUT, _("&Create shortcut"));
			menu.AppendSeparator();
			menu.Append(ID_GAME_COPY_NAME, _("&Copy Title Name"));
			menu.Append(ID_GAME_COPY_ID, _("&Copy Title ID"));
			menu.Append(ID_GAME_COPY_IMAGE, _("&Copy Title Image"));
			menu.AppendSeparator();
		}
	}

	menu.Append(ID_LIST_REFRESH, _("&Refresh game list"))->Enable(!CafeTitleList::IsScanning());
	menu.AppendSeparator();
	menu.AppendRadioItem(ID_LIST_STYLE_LIST, _("Style: &List"))->Check(m_style == Style::kList);
	menu.AppendRadioItem(ID_LIST_STYLE_ICONS, _("Style: &Icons"))->Check(m_style == Style::kIcons);
	menu.AppendRadioItem(ID_LIST_STYLE_SMALL_ICONS, _("Style: &Small Icons"))->Check(m_style == Style::kSmallIcons);

	menu.Bind(wxEVT_MENU, [this, titleId](wxCommandEvent& evt) { OnGameCommand(evt.GetId(), titleId); });
	PopupMenu(&menu);
}

void wxGameList::OnGameCommand(int id, uint64 titleId)
{
	switch (id)
	{
	case ID_LIST_REFRESH: ReloadGameEntries(); return;
	case ID_LIST_STYLE_LIST: SetStyle(Style::kList); return;
	case ID_LIST_STYLE_ICONS: SetStyle(Style::kIcons); return;
	case ID_LIST_STYLE_SMALL_ICONS: SetStyle(Style::kSmallIcons); return;
	default: break;
	}
	if (titleId == 0)
		return;
	GameInfo2 game = CafeTitleList::GetGameInfo(titleId);
	if (!game.IsValid())
		return;

	switch (id)
	{
	case ID_GAME_START:
		MainWindow::RequestLaunchGame(game.GetBase().GetPath(), wxLaunchGameEvent::INITIATED_BY::GAME_LIST);
		break;
	case ID_GAME_FAVORITE:
		GetConfig().SetGameListFavorite(titleId, !GetConfig().IsGameListFavorite(titleId));
		SortRows();
		RecolourRows();
		SaveConfig(true);
		break;
	case ID_GAME_RENAME:
	{
		std::string current;
		if (!GetConfig().GetGameListCustomName(titleId, current))
			current.clear();
		wxTextEntryDialog ask(this, wxEmptyString, _("Enter a custom game title"), wxString::FromUTF8(current));
		if (ask.ShowModal() != wxID_OK)
			break;
		GetConfig().SetGameListCustomName(titleId, ask.GetValue().utf8_string());
		m_nameCache.clear();
		GetConfigHandle().Save();
		if (const long row = FindListItemByTitleId(titleId); row != wxNOT_FOUND && m_style == Style::kList)
			SetItem(row, kColName, wxString::FromUTF8(TitleName(titleId)));
		SortRows();
		RecolourRows();
		break;
	}
	case ID_GAME_DIR:
		OpenFolder(FolderOf(game.GetBase().GetPath()));
		break;
	case ID_GAME_SAVE_DIR:
		OpenFolder(game.GetSaveFolder());
		break;
	case ID_GAME_UPDATE_DIR:
		OpenFolder(FolderOf(game.GetUpdate().GetPath()));
		break;
	case ID_GAME_DLC_DIR:
		OpenFolder(FolderOf(game.GetAOC().front().GetPath()));
		break;
	case ID_GAME_REMOVE_CACHES:
		RemoveShaderCaches(ExistingShaderCaches(game.GetBaseTitleId()), game.GetTitleName());
		break;
	case ID_GAME_GRAPHIC_PACKS:
	{
		wxTitleIdEvent open(wxEVT_OPEN_GRAPHIC_PACK, titleId);
		ProcessWindowEvent(open);
		break;
	}
	case ID_GAME_PROFILE:
		(new GameProfileWindow(GetParent(), titleId))->Show();
		break;
	case ID_GAME_SHORTCUT:
		CreateShortcut(game);
		break;
	case ID_GAME_COPY_NAME:
		CopyToClipboard(new wxTextDataObject(wxString::FromUTF8(game.GetTitleName())));
		break;
	case ID_GAME_COPY_ID:
		CopyToClipboard(new wxTextDataObject(fmt::format("{:016x}", game.GetBaseTitleId())));
		break;
	case ID_GAME_COPY_IMAGE:
	{
		int icon, iconSmall;
		if (QueryIconForTitle(titleId, icon, iconSmall))
			CopyToClipboard(new wxBitmapDataObject(m_largeIcons.GetIcon(icon)));
		break;
	}
	default:
		break;
	}
}

void wxGameList::RemoveShaderCaches(const std::vector<fs::path>& files, const std::string& titleName)
{
	wxMessageDialog ask(this, formatWxString(_("Remove the shader caches for {}?"), titleName), _("Remove shader caches"), wxCENTRE | wxYES_NO | wxICON_EXCLAMATION);
	ask.SetYesNoLabels(_("Yes"), _("No"));
	if (ask.ShowModal() != wxID_YES)
		return;

	std::vector<std::string> failures;
	for (const fs::path& f : files)
	{
		std::error_code ec;
		if (!fs::remove(f, ec))
			failures.emplace_back(fmt::format("{} : {}", f.string(), ec.message()));
	}
	if (failures.empty())
		wxMessageDialog(this, _("The shader caches were removed!"), _("Shader caches removed"), wxCENTRE | wxOK | wxICON_INFORMATION).ShowModal();
	else
		wxMessageDialog(this, formatWxString(_("Failed to remove the shader caches:\n{}"), fmt::join(failures, "\n")), _("Error"), wxCENTRE | wxOK | wxICON_ERROR).ShowModal();
}

// ---------------------------------------------------------------------------
// shortcuts

#if BOOST_OS_LINUX || BOOST_OS_BSD
void wxGameList::CreateShortcut(GameInfo2& gameInfo)
{
	const TitleId titleId = gameInfo.GetBaseTitleId();
	const wxString titleName = wxString::FromUTF8(gameInfo.GetTitleName());
	const fs::path exePath = ActiveSettings::GetExecutablePath();
	const char* flatpakId = getenv("FLATPAK_ID");

	wxFileDialog where(this, _("Choose desktop entry location"), "~/.local/share/applications", wxString::Format("%s.desktop", titleName),
					   "Desktop file (*.desktop)|*.desktop", wxFD_SAVE | wxFD_CHANGE_DIR | wxFD_OVERWRITE_PROMPT);
	if (where.ShowModal() == wxID_CANCEL)
		return;
	const wxString target = where.GetPath();

	std::optional<fs::path> iconPath;
	int icon, iconSmall;
	if (QueryIconForTitle(titleId, icon, iconSmall))
		iconPath = SaveIconPng(m_largeIcons, icon, ActiveSettings::GetUserDataPath("icons"), titleId);
	else
		cemuLog_log(LogType::Force, "Icon hasn't loaded");

	// Exec needs quoting for paths with spaces; Icon must not be quoted
	const std::string exec = flatpakId ? fmt::format("/usr/bin/flatpak run {0} --title-id {1:016x}", flatpakId, titleId)
									   : fmt::format("{0:?} --title-id {1:016x}", _pathToUtf8(exePath), titleId);
	std::string entry = fmt::format("[Desktop Entry]\n"
									"Name={0}\n"
									"Comment=Play {0} on PixelU\n"
									"Exec={1}\n"
									"Icon={2}\n"
									"Terminal=false\n"
									"Type=Application\n"
									"Categories=Game;\n",
									titleName.utf8_string(), exec, _pathToUtf8(iconPath.value_or("")));
	if (flatpakId)
		entry += fmt::format("X-Flatpak={}\n", flatpakId);

	std::ofstream out(target.utf8_string());
	if (!out.good())
	{
		wxMessageBox(formatWxString(_("Failed to save desktop entry to {}"), target.utf8_string()), _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		return;
	}
	out << entry;
}
#elif BOOST_OS_MACOS
void wxGameList::CreateShortcut(GameInfo2& gameInfo)
{
	const TitleId titleId = gameInfo.GetBaseTitleId();
	const wxString titleName = wxString::FromUTF8(gameInfo.GetTitleName());
	const fs::path exePath = ActiveSettings::GetExecutablePath();

	wxFileDialog where(this, _("Choose shortcut location"), "~/Applications", wxString::Format("%s.app", titleName),
					   "Application (*.app)|*.app", wxFD_SAVE | wxFD_CHANGE_DIR | wxFD_OVERWRITE_PROMPT);
	if (where.ShowModal() == wxID_CANCEL)
		return;
	const wxString target = where.GetPath();

	// bundle layout: Contents/{Info.plist, MacOS/run.sh, Resources/shortcut.icns}
	const fs::path bundle = target.utf8_string();
	if (!fs::create_directories(bundle))
	{
		cemuLog_log(LogType::Force, "Failed to create app directory");
		return;
	}
	const fs::path plistPath = bundle / "Contents/Info.plist";
	const fs::path scriptPath = bundle / "Contents/MacOS/run.sh";
	const fs::path icnsPath = bundle / "Contents/Resources/shortcut.icns";
	if (!(fs::create_directories(scriptPath.parent_path()) && fs::create_directories(icnsPath.parent_path())))
	{
		cemuLog_log(LogType::Force, "Failed to create app shortcut directories");
		return;
	}

	std::optional<fs::path> pngPath;
	int icon, iconSmall;
	if (QueryIconForTitle(titleId, icon, iconSmall))
		pngPath = SaveIconPng(m_largeIcons, icon, fs::temp_directory_path(), titleId);
	else
		cemuLog_log(LogType::Force, "Icon hasn't loaded");

	const std::string script = fmt::format("#!/bin/zsh\n\n{0:?} --title-id {1:016x}", _pathToUtf8(exePath), titleId);
	const std::string version = std::to_string(gameInfo.GetVersion());
	const std::string plist = fmt::format(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
		"<plist version=\"1.0\">\n"
		"<dict>\n"
		"\t<key>CFBundleDisplayName</key>\n"
		"\t<string>{0}</string>\n"
		"\t<key>CFBundleExecutable</key>\n"
		"\t<string>run.sh</string>\n"
		"\t<key>CFBundleIconFile</key>\n"
		"\t<string>shortcut.icns</string>\n"
		"\t<key>CFBundleName</key>\n"
		"\t<string>{0}</string>\n"
		"\t<key>CFBundlePackageType</key>\n"
		"\t<string>APPL</string>\n"
		"\t<key>CFBundleSignature</key>\n"
		"\t<string>\?\?\?\?</string>\n"
		"\t<key>LSApplicationCategoryType</key>\n"
		"\t<string>public.app-category.games</string>\n"
		"\t<key>CFBundleShortVersionString</key>\n"
		"\t<string>{1}</string>\n"
		"\t<key>CFBundleVersion</key>\n"
		"\t<string>{1}</string>\n"
		"</dict>\n"
		"</plist>\n",
		gameInfo.GetTitleName(), version);

	std::ofstream plistOut(plistPath);
	std::ofstream scriptOut(scriptPath);
	if (!plistOut.good() || !scriptOut.good())
	{
		wxMessageBox(formatWxString(_("Failed to save app shortcut to {}"), target.utf8_string()), _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		return;
	}
	plistOut << plist;
	scriptOut << script;
	scriptOut.close();
	fs::permissions(scriptPath, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);

	if (!pngPath)
	{
		cemuLog_log(LogType::Force, "Icon not found");
		return;
	}
	if (!WritePngAsIcns(*pngPath, icnsPath))
	{
		cemuLog_log(LogType::Force, "Failed to convert icon to icns");
		return;
	}
	fs::remove(*pngPath);
}
#elif BOOST_OS_WINDOWS
void wxGameList::CreateShortcut(GameInfo2& gameInfo)
{
	const TitleId titleId = gameInfo.GetBaseTitleId();
	const wxString titleName = wxString::FromUTF8(gameInfo.GetTitleName());
	const fs::path exePath = ActiveSettings::GetExecutablePath();

	// default to the user's start menu programs folder
	PWSTR programsFolder = nullptr;
	SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programsFolder);
	wxFileDialog where(this, _("Choose shortcut location"), programsFolder, wxString::Format("%s.lnk", titleName),
					   "Shortcut (*.lnk)|*.lnk", wxFD_SAVE | wxFD_CHANGE_DIR | wxFD_OVERWRITE_PROMPT);
	CoTaskMemFree(programsFolder);
	if (where.ShowModal() == wxID_CANCEL)
		return;
	const wxString target = where.GetPath();

	int icon, iconSmall;
	if (!QueryIconForTitle(titleId, icon, iconSmall))
	{
		cemuLog_log(LogType::Force, "Icon hasn't loaded");
		return;
	}
	std::optional<fs::path> icoPath;
	{
		const fs::path folder = ActiveSettings::GetUserDataPath("icons");
		if (!fs::exists(folder) && !fs::create_directories(folder))
		{
			cemuLog_log(LogType::Force, "Failed to create icon directory");
			return;
		}
		icoPath = folder / fmt::format("{:016x}.ico", titleId);
		wxFileOutputStream stream(icoPath->wstring());
		wxImage image = wxBitmap(m_largeIcons.GetIcon(icon)).ConvertToImage();
		wxICOHandler ico;
		if (!ico.SaveFile(&image, stream, false))
		{
			icoPath.reset();
			cemuLog_log(LogType::Force, "Icon failed to save");
		}
	}

	Microsoft::WRL::ComPtr<IShellLinkW> link;
	HRESULT hr = CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
	if (SUCCEEDED(hr))
	{
		const wxString description = wxString::Format("Play %s on PixelU", titleName);
		const wxString arguments = wxString::Format("-t %016llx", (unsigned long long)titleId);
		link->SetPath(exePath.wstring().c_str());
		link->SetDescription(description.wc_str());
		link->SetArguments(arguments.wc_str());
		link->SetWorkingDirectory(exePath.parent_path().wstring().c_str());
		link->SetIconLocation((icoPath ? *icoPath : exePath).wstring().c_str(), 0);

		Microsoft::WRL::ComPtr<IPersistFile> file;
		hr = link.As(&file);
		if (SUCCEEDED(hr))
			hr = file->Save(target.wc_str(), TRUE);
	}
	if (FAILED(hr))
		wxMessageBox(formatWxString(_("Failed to save shortcut to {}"), target), _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
}
#endif
