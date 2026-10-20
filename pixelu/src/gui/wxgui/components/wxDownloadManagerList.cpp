// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/components/wxDownloadManagerList.h"

#include "wxHelper.h"
#include "wxgui/helpers/wxHelpers.h"
#include "util/helpers/SystemException.h"
#include "Cafe/TitleList/GameInfo.h"
#include "wxgui/components/wxGameList.h"
#include "wxgui/helpers/wxCustomEvents.h"

#include <wx/wupdlock.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/panel.h>

#include <functional>

#include "config/ActiveSettings.h"
#include "Cemu/Tools/DownloadManager/DownloadManager.h"
#include "Cafe/TitleList/TitleId.h"
#include "wxgui/MainWindow.h"

// ==== dlsort begin ====
// Display order of the download list (no wx types, so it can be tested on its own).
namespace dlsort
{
	enum Key
	{
		ByTitleId = 0,
		ByName = 1,
		ByVersion = 2,
		ByType = 3,
		ByProgress = 4,
	};

	// the clicked column decides first, then name, type, version, title id and progress
	inline std::vector<int> KeyOrder(int clicked)
	{
		std::vector<int> order{clicked};
		for (int k : {ByName, ByType, ByVersion, ByTitleId, ByProgress})
		{
			if (k != clicked)
				order.push_back(k);
		}
		return order;
	}

	template<typename T>
	int Three(const T& a, const T& b)
	{
		return a < b ? -1 : (b < a ? 1 : 0);
	}

	// strict "a before b": visible rows first, then the keys in order; columns
	// without a key (status) are skipped
	template<typename TEntry, typename TNameCmp>
	bool Before(const std::vector<int>& order, bool visibleA, const TEntry& a, bool visibleB, const TEntry& b, TNameCmp nameCmp)
	{
		if (visibleA != visibleB)
			return visibleA;
		for (int key : order)
		{
			int c = 0;
			switch (key)
			{
			case ByTitleId: c = Three(a.titleId, b.titleId); break;
			case ByName: c = nameCmp(a, b); break;
			case ByType: c = Three((int)a.type, (int)b.type); break;
			case ByVersion: c = Three(a.version, b.version); break;
			case ByProgress: c = Three(a.progress, b.progress); break;
			default: break;
			}
			if (c != 0)
				return c < 0;
		}
		return false;
	}
}
// ==== dlsort end ====

namespace
{
	enum
	{
		ID_RETRY = wxID_HIGHEST + 1,
		ID_DOWNLOAD,
		ID_RESUME,
	};

	bool IsBusyStatus(wxDownloadManagerList::TitleDownloadStatus s)
	{
		using S = wxDownloadManagerList::TitleDownloadStatus;
		return s == S::Downloading || s == S::Verifying || s == S::Installing;
	}
}

wxDownloadManagerList::wxDownloadManagerList(wxWindow* parent, wxWindowID id)
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
		{ColumnName, _("Name"), 260},
		{ColumnVersion, _("Version"), 55},
		{ColumnType, _("Type"), 60},
		{ColumnProgress, _("Progress"), wxLIST_AUTOSIZE_USEHEADER},
		{ColumnStatus, _("Status"), 240},
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
	Bind(wxEVT_CONTEXT_MENU, &wxDownloadManagerList::OnContextMenu, this);
	Bind(wxEVT_REMOVE_ITEM, [this](wxCommandEvent& evt) { RemoveRow(evt.GetInt()); });
	Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent& evt) {
		evt.Skip();
		// downloaded games show up in the game list after a full refresh
		g_mainFrame->RequestGameListRefresh();
	});

	ShowSortIndicator(ColumnName);
}

// ---------------------------------------------------------------------------
// entries

boost::optional<wxDownloadManagerList::TitleEntry&> wxDownloadManagerList::EntryAtRow(long row) const
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

boost::optional<wxDownloadManagerList::TitleEntry&> wxDownloadManagerList::EntryById(uint64 titleId, uint16 version) const
{
	for (const auto& item : m_data)
	{
		if (item->entry.titleId == titleId && item->entry.version == version)
			return item->entry;
	}
	return {};
}

boost::optional<const wxDownloadManagerList::TitleEntry&> wxDownloadManagerList::GetSelectedTitleEntry() const
{
	if (auto e = EntryAtRow(GetFirstSelected()))
		return *e;
	return {};
}

boost::optional<wxDownloadManagerList::TitleEntry&> wxDownloadManagerList::GetSelectedTitleEntry()
{
	return EntryAtRow(GetFirstSelected());
}

void wxDownloadManagerList::AddOrUpdateTitle(TitleEntryData_t* obj)
{
	const TitleEntry& update = obj->GetData();
	if (auto known = EntryById(update.titleId, update.version))
	{
		*known = update;
		RefreshPage();
		return;
	}
	m_data.emplace_back(std::make_unique<ItemData>(true, update));
	m_sorted.emplace_back(*m_data.back());
	SetItemCount(m_data.size());
	// a new row has to go through the current filter and order
	Filter2(m_filterShowTitles, m_filterShowUpdates, m_filterShowInstalled);
	SortEntries();
}

void wxDownloadManagerList::RemoveRow(long row)
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

void wxDownloadManagerList::ShowRows(size_t count)
{
	SetItemCount(count);
	RefreshPage();
}

size_t wxDownloadManagerList::GetCountByType(EntryType type) const
{
	return std::count_if(m_data.begin(), m_data.end(), [type](const auto& p) { return p->entry.type == type; });
}

void wxDownloadManagerList::ClearItems()
{
	m_sorted.clear();
	m_data.clear();
	ShowRows(0);
}

void wxDownloadManagerList::AutosizeColumns()
{
	wxAutosizeColumns(this, ColumnTitleId, ColumnMAX - 1);
}

void wxDownloadManagerList::RefreshPage()
{
	const long rows = GetItemCount();
	if (rows <= 0)
		return;
	const long top = GetTopItem();
	RefreshItems(top, std::min(rows - 1, top + GetCountPerPage() + 1));
}

// ---------------------------------------------------------------------------
// display

wxString wxDownloadManagerList::TypeName(EntryType type)
{
	switch (type)
	{
	case EntryType::Base: return _("base");
	case EntryType::Update: return _("update");
	case EntryType::DLC: return _("DLC");
	}
	return std::to_string((int)type);
}

wxString wxDownloadManagerList::ColumnText(const TitleEntry& entry, ItemColumn column)
{
	using S = TitleDownloadStatus;
	switch (column)
	{
	case ColumnTitleId:
		return formatWxString("{:08x}-{:08x}", (uint32)(entry.titleId >> 32), (uint32)entry.titleId);
	case ColumnName:
		return entry.name;
	case ColumnType:
		return TypeName(entry.type);
	case ColumnVersion:
		// v0 of a game or DLC is not worth showing
		if (entry.version == 0 && (entry.type == EntryType::Base || entry.type == EntryType::DLC))
			return "";
		return formatWxString("v{}", entry.version);
	case ColumnProgress:
		if (entry.status == S::Downloading)
			return entry.progress >= 1000 ? wxString("100%") : formatWxString("{:.1f}%", (float)entry.progress / 10.0f);
		if (entry.status == S::Installing || entry.status == S::Checking || entry.status == S::Verifying)
			return formatWxString("{0}/{1}", entry.progress, entry.progressMax);
		return "";
	case ColumnStatus:
		if (entry.isPaused)
			return _("Paused");
		switch (entry.status)
		{
		case S::Available:
			if (entry.progress == 1)
				return _("Not installed (Partially downloaded)");
			if (entry.progress == 2)
				return _("Update available");
			return _("Not installed");
		case S::Initializing: return _("Initializing");
		case S::Checking: return _("Checking");
		case S::Queued: return _("Queued");
		case S::Downloading: return _("Downloading");
		case S::Verifying: return _("Verifying");
		case S::Installing: return _("Installing");
		case S::Installed: return _("Installed");
		case S::Error:
		{
			wxString text = _("Error:");
			text.append(" ");
			text.append(entry.errorMsg);
			return text;
		}
		default: return "Unknown status";
		}
	default:
		break;
	}
	return wxEmptyString;
}

wxString wxDownloadManagerList::OnGetItemText(long item, long column) const
{
	if (item >= GetItemCount())
		return wxEmptyString;
	if (auto e = EntryAtRow(item))
		return ColumnText(*e, (ItemColumn)column);
	return wxEmptyString;
}

wxItemAttr* wxDownloadManagerList::OnGetItemAttr(long item) const
{
	// busy rows red, finished downloads green, errors blue; otherwise alternate shading
	const bool dark = wxSystemSettings::GetAppearance().IsDark();
	wxColour background;
	if (auto e = EntryAtRow(item))
	{
		if (IsBusyStatus(e->status))
			background = dark ? wxColour(80, 40, 40) : wxColour(0xFFE0E0);
		else if (e->status == TitleDownloadStatus::Installed && e->isPackage)
			background = dark ? wxColour(40, 80, 40) : wxColour(0xE0FFE0);
		else if (e->status == TitleDownloadStatus::Error)
			background = dark ? wxColour(40, 40, 80) : wxColour(0xCCCCF2);
	}
	if (!background.IsOk())
	{
		if (item % 2 == 0)
			return nullptr;
		background = wxHelper::CalculateAccentColour(GetBackgroundColour());
	}
	m_attr = wxItemAttr(GetTextColour(), background, GetFont());
	return &m_attr;
}

void wxDownloadManagerList::SortEntries(int column)
{
	bool ascending;
	if (column == -1)
	{
		column = GetSortIndicator();
		if (column == -1)
			column = ColumnName;
		ascending = IsAscendingSortIndicator();
	}
	else
		ascending = GetUpdatedAscendingSortIndicator(column);

	// list columns map onto sort keys one to one (ColumnStatus has none)
	const std::vector<int> order = dlsort::KeyOrder(column);
	auto nameCmp = [](const TitleEntry& a, const TitleEntry& b) { return a.name.CmpNoCase(b.name); };
	std::sort(m_sorted.begin(), m_sorted.end(), [&](const std::reference_wrapper<ItemData>& x, const std::reference_wrapper<ItemData>& y) {
		const ItemData& a = ascending ? x.get() : y.get();
		const ItemData& b = ascending ? y.get() : x.get();
		return dlsort::Before(order, a.visible, a.entry, b.visible, b.entry, nameCmp);
	});
	ShowSortIndicator(column, ascending);
	RefreshPage();
}

void wxDownloadManagerList::Filter(const wxString& filter)
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
		{"VERSION:", ColumnVersion},
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
			match = [](const TitleEntry&) { return false; }; // no error flag per entry
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

void wxDownloadManagerList::Filter2(bool showTitles, bool showUpdates, bool showInstalled)
{
	m_filterShowTitles = showTitles;
	m_filterShowUpdates = showUpdates;
	m_filterShowInstalled = showInstalled;

	size_t shown = 0;
	for (auto& item : m_data)
	{
		const bool isUpdate = TitleIdParser(item->entry.titleId).IsBaseTitleUpdate();
		const bool installedOk = showInstalled || item->entry.status != TitleDownloadStatus::Installed;
		item->visible = (isUpdate ? showUpdates : showTitles) && installedOk;
		shown += item->visible ? 1 : 0;
	}
	ShowRows(shown);
}

// ---------------------------------------------------------------------------
// downloads

void wxDownloadManagerList::SetCurrentDownloadMgr(DownloadManager* dlMgr)
{
	std::lock_guard lock(m_dlMgrMutex);
	m_dlMgr = dlMgr;
}

void wxDownloadManagerList::StartDownload(const TitleEntry& entry)
{
	std::lock_guard lock(m_dlMgrMutex);
	if (m_dlMgr)
		m_dlMgr->initiateDownload(entry.titleId, entry.version);
}

void wxDownloadManagerList::OnContextMenu(wxContextMenuEvent& /*event*/)
{
	const auto entry = EntryAtRow(GetFirstSelected());
	if (!entry)
		return;

	wxMenu menu;
	// one action per state; pausing a running download is not offered
	if (entry->isPaused)
		menu.Append(ID_RESUME, _("&Resume"));
	else if (entry->status == TitleDownloadStatus::Error)
		menu.Append(ID_RETRY, _("&Retry"));
	else if (entry->status == TitleDownloadStatus::Available)
		menu.Append(ID_DOWNLOAD, _("&Download"));
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent& evt) {
		const auto selected = EntryAtRow(GetFirstSelected());
		if (!selected)
			return;
		if (evt.GetId() == ID_RESUME || evt.GetId() == ID_RETRY || evt.GetId() == ID_DOWNLOAD)
			StartDownload(*selected);
	});
	PopupMenu(&menu);
}
