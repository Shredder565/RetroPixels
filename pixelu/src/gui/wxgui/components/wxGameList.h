// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "config/CemuConfig.h"
#include "Cafe/TitleList/TitleId.h"

#include <wx/listctrl.h>
#include <wx/timer.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <Cafe/TitleList/GameInfo.h>

#include "wxHelper.h"
#include "util/helpers/Semaphore.h"

// command event that names a title
class wxTitleIdEvent : public wxCommandEvent
{
public:
	wxTitleIdEvent(int type, uint64 title_id)
		: wxCommandEvent(type), m_title_id(title_id) {}

	[[nodiscard]] uint64 GetTitleId() const { return m_title_id; }

private:
	uint64 m_title_id;
};

wxDECLARE_EVENT(wxEVT_OPEN_SETTINGS, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_OPEN_GRAPHIC_PACK, wxTitleIdEvent);
wxDECLARE_EVENT(wxEVT_GAMELIST_BEGIN_UPDATE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_GAMELIST_END_UPDATE, wxCommandEvent);

// The main window's list of installed games. Rows follow the CafeTitleList
// callbacks (batched by a short timer); icons are decoded on a worker thread.
class wxGameList : public wxListView
{
	friend class MainWindow;
public:
	enum class Style
	{
		kList,
		kIcons,
		kSmallIcons
	};

	wxGameList(wxWindow* parent, wxWindowID id = wxID_ANY);
	~wxGameList();

	void SetStyle(Style style, bool save = true);

	void LoadConfig();
	void SaveConfig(bool flush = false);
	bool IsVisible(long item) const;

	void ReloadGameEntries();
	void DeleteCachedStrings();

	void CreateShortcut(GameInfo2& gameInfo);

	long FindListItemByTitleId(uint64 title_id) const;

	void OnClose(wxCloseEvent& event);

private:
	enum Column : int
	{
		kColHidden = 0, // keeps column 0 (which cannot hold an image on every platform) out of sight
		kColIcon,
		kColName,
		kColVersion,
		kColDlc,
		kColPlayTime,
		kColLastPlayed,
		kColRegion,
		kColTitleId,
		kColCount,
	};

	struct DecodedIcon
	{
		TitleId titleId;
		wxImage largeImage;
		wxImage smallImage; // ("small" is a macro in the Windows headers)
	};

	static long StyleFlags(Style style);
	static int DefaultColumnWidth(int column);

	// rows
	void RegisterTitleListCallback();
	void OnTitleListChanged(wxTitleIdEvent& event);
	void FlushPendingTitles(wxTimerEvent& event);
	void FillRow(long row, GameInfo2& game, TitleId baseId, bool newRow, int icon, int iconSmall);
	long InsertPosition(TitleId titleId);
	std::string TitleName(uint64 titleId);
	void RecolourRows();
	void SortRows(int column = -1);
	std::weak_ordering CompareTitles(int column, uint64 a, uint64 b);

	// icons
	void IconWorker();
	void TakeDecodedIcons();
	bool QueryIconForTitle(TitleId titleId, int& icon, int& iconSmall);

	// columns
	int LastVisibleColumn() const;
	void ApplyGameListColumnWidths();
	void AdjustLastColumnWidth();
	void OnColumnClick(wxListEvent& event);
	void OnColumnRightClick(wxListEvent& event);
	void OnColumnBeginResize(wxListEvent& event);
	void OnColumnResize(wxListEvent& event);
	void OnSize(wxSizeEvent& event);

	// interaction
	void OnKeyDown(wxListEvent& event);
	void OnItemActivated(wxListEvent& event);
	void OnContextMenu(wxContextMenuEvent& event);
	void OnGameCommand(int id, uint64 titleId);
	void RemoveShaderCaches(const std::vector<fs::path>& files, const std::string& titleName);

	std::atomic_bool m_exit = false;
	Style m_style = Style::kList;
	std::optional<TitleId> m_pendingSelection;
	uint64 m_callbackIdTitleList = 0;

	// titles reported by the title list, waiting for the batch timer
	std::vector<TitleId> m_pendingTitles;
	wxTimer m_batchTimer;

	std::map<TitleId, std::string> m_nameCache;

	inline static constexpr int kListIconWidth = 64;
	inline static constexpr int kIconWidth = 128;
	wxImageList m_largeIcons = wxImageList(kIconWidth, kIconWidth, false, 1);
	wxImageList m_smallIcons = wxImageList(kListIconWidth, kListIconWidth, false, 1);
	std::map<TitleId, std::pair<int, int>> m_iconIndex; // UI thread only: large and small image list index

	// icon worker
	std::thread m_iconThread;
	std::atomic_bool m_iconThreadRun{false};
	CounterSemaphore m_iconRequests;
	std::mutex m_iconMutex;
	std::deque<TitleId> m_iconQueue;            // guarded by m_iconMutex
	std::vector<DecodedIcon> m_decodedIcons;    // guarded by m_iconMutex
	std::set<TitleId> m_iconAttempted;          // worker thread only
};
