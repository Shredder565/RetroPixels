// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "wxgui/helpers/wxCustomData.h"
#include "config/CemuConfig.h"

#include <wx/listctrl.h>

#include <boost/optional.hpp> // optional references
#include <utility>
#include <vector>

// Virtual list of installed titles, updates, DLC and saves for the title
// manager, fed by the title and save list callbacks.
class wxTitleManagerList : public wxListView
{
	friend class TitleManager;
public:
	wxTitleManagerList(wxWindow* parent, wxWindowID id = wxID_ANY);
	~wxTitleManagerList();

	enum ItemColumn
	{
		ColumnTitleId = 0,
		ColumnName,
		ColumnType,
		ColumnVersion,
		ColumnRegion,
		ColumnFormat,
		ColumnLocation,

		ColumnMAX,
	};

	enum class EntryType
	{
		Base,
		Update,
		Dlc,
		Save,
		System,
	};

	enum class EntryFormat
	{
		Folder,
		WUD,
		NUS,
		WUA,
		WUHB,
	};

	// sorts by the given column; -1 re-applies the current sort (title id by default)
	void SortEntries(int column);
	void RefreshPage();
	// plain text matches name, title id or type; "TITLEID:", "NAME:", "TYPE:",
	// "REGION:", "VERSION:" and "FORMAT:" prefixes match a single column
	void Filter(const wxString& filter);
	[[nodiscard]] size_t GetCountByType(EntryType type) const;
	void ClearItems();
	void AutosizeColumns();

	struct TitleEntry
	{
		TitleEntry(const EntryType& type, const EntryFormat& format, fs::path path)
			: type(type), format(format), path(std::move(path)) {}

		EntryType type;
		EntryFormat format;
		fs::path path;

		int icon = -1;
		uint64 location_uid;
		uint64 title_id;
		wxString name;
		uint32_t version = 0;
		CafeConsoleRegion region;

		std::vector<uint32> persistent_ids; // saves only
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
	void AddTitle(TitleEntryData_t* obj);

	boost::optional<TitleEntry&> EntryAtRow(long row) const;
	boost::optional<TitleEntry&> EntryByUid(uint64 uid) const;
	wxString OnGetItemText(long item, long column) const override;
	wxItemAttr* OnGetItemAttr(long item) const override;
	static wxString ColumnText(const TitleEntry& entry, ItemColumn column);
	static wxString TypeName(EntryType type);
	void ShowRows(size_t count);

	void QueueTitleEvent(struct CafeTitleListCallbackEvent* evt);
	void QueueSaveEvent(struct CafeSaveListCallbackEvent* evt);
	void OnTitleFoundEvent(wxCommandEvent& event);
	void OnTitleRemovedEvent(wxCommandEvent& event);

	void OnContextMenu(wxContextMenuEvent& event);
	void OnMenuCommand(int id);
	bool IsBusy();
	void DeleteWithConfirmation(long row, const TitleEntry& entry);
	void RemoveRow(long row);
	void RemoveEntry(const TitleEntry* entry);
	void ConvertToWua(uint64 titleId, uint64 clickedUid);

	std::vector<std::unique_ptr<ItemData>> m_data;          // owns the entries, in arrival order
	std::vector<std::reference_wrapper<ItemData>> m_sorted;  // display order; hidden ones are skipped
	std::future<bool> m_context_worker;
	mutable wxItemAttr m_rowAttr[2];

	uint64 m_callbackIdTitleList;
	uint64 m_callbackIdSaveList;
};
