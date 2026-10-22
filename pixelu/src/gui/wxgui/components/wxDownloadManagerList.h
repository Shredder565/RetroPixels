// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "wxgui/helpers/wxCustomData.h"
#include "config/CemuConfig.h"

#include <wx/listctrl.h>

#include <boost/optional.hpp> // optional references
#include <utility>
#include <vector>

// Virtual list of the titles offered by the download manager (eShop titles
// owned by the account) with their download / install state.
class wxDownloadManagerList : public wxListView
{
	friend class TitleManager;
public:
	wxDownloadManagerList(wxWindow* parent, wxWindowID id = wxID_ANY);

	enum ItemColumn
	{
		ColumnTitleId = 0,
		ColumnName,
		ColumnVersion,
		ColumnType,
		ColumnProgress,
		ColumnStatus,

		ColumnMAX,
	};

	enum class EntryType
	{
		Base,
		Update,
		DLC,
	};

	enum class TitleDownloadStatus
	{
		None,
		Available,    // can be downloaded
		Error,
		Queued,
		Initializing, // fetching and parsing the TMD
		Checking,     // looking for previously downloaded files
		Downloading,
		Verifying,    // hashing downloaded files
		Installing,
		Installed,
	};

	void SortEntries(int column = -1);
	void RefreshPage();
	void Filter(const wxString& filter);
	// shows games and/or updates, optionally hiding installed ones
	void Filter2(bool showTitles, bool showUpdates, bool showInstalled);
	[[nodiscard]] size_t GetCountByType(EntryType type) const;
	void ClearItems();
	void AutosizeColumns();

	struct TitleEntry
	{
		TitleEntry(const EntryType& type, bool isPackage, uint64 titleId, uint16 version, bool isPaused)
			: type(type), isPackage(isPackage), titleId(titleId), version(version), isPaused(isPaused) {}

		EntryType type;

		bool isPaused{};
		int icon = -1;
		bool isPackage; // a download in progress or done, as opposed to an offered title
		uint64 titleId;
		wxString name;
		uint32_t version{ 0 };
		uint32 progress; // downloading: 1/10th percent, installing/checking/verifying: files done; offered titles: 1 = partially downloaded, 2 = update available
		uint32 progressMax{ 0 };
		CafeConsoleRegion region;

		TitleDownloadStatus status = TitleDownloadStatus::None;
		std::string errorMsg;

		bool operator==(const TitleEntry& e) const { return titleId == e.titleId && version == e.version && type == e.type; }
		bool operator!=(const TitleEntry& e) const { return !(*this == e); }
	};
	boost::optional<const TitleEntry&> GetSelectedTitleEntry() const;

private:
	using TitleEntryData_t = wxCustomData<TitleEntry>;

	struct ItemData
	{
		ItemData(bool visible, const TitleEntry& entry)
			: visible(visible), entry(entry) {}

		bool visible;
		TitleEntry entry;
	};

	boost::optional<TitleEntry&> GetSelectedTitleEntry();
	void AddOrUpdateTitle(TitleEntryData_t* obj);
	void SetCurrentDownloadMgr(class DownloadManager* dlMgr);

	boost::optional<TitleEntry&> EntryAtRow(long row) const;
	boost::optional<TitleEntry&> EntryById(uint64 titleId, uint16 version) const;
	wxString OnGetItemText(long item, long column) const override;
	wxItemAttr* OnGetItemAttr(long item) const override;
	static wxString ColumnText(const TitleEntry& entry, ItemColumn column);
	static wxString TypeName(EntryType type);
	void ShowRows(size_t count);
	void RemoveRow(long row);

	void OnContextMenu(wxContextMenuEvent& event);
	void StartDownload(const TitleEntry& entry);

	std::vector<std::unique_ptr<ItemData>> m_data;          // owns the entries
	std::vector<std::reference_wrapper<ItemData>> m_sorted;  // display order; hidden ones are skipped

	bool m_filterShowTitles = true;
	bool m_filterShowUpdates = true;
	bool m_filterShowInstalled = true;

	DownloadManager* m_dlMgr{};
	std::mutex m_dlMgrMutex;

	mutable wxItemAttr m_attr;
};
