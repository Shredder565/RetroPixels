// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <mutex>
#include <thread>
#include <atomic>
#include <future>
#include <functional>

#include <wx/listctrl.h>
#include <wx/frame.h>

#include "Cafe/HW/MMU/MMU.h"
#include "util/helpers/helpers.h"
#include "wxgui/helpers/wxCustomEvents.h"

class wxComboBox;
class wxDataViewEvent;
class wxDataViewListCtrl;
class wxStaticText;
class wxTimer;
class wxTimerEvent;

// value types the searcher understands (strings only as stored entries, not as a search)
enum SearchDataType
{
	SearchDataType_None,
	SearchDataType_String,
	SearchDataType_Float,
	SearchDataType_Double,
	SearchDataType_Int8,
	SearchDataType_Int16,
	SearchDataType_Int32,
	SearchDataType_Int64,
};

struct TableEntry_t
{
	uint32 address;
	const char description[32];
	SearchDataType type;
	bool freeze;
	uint16 dataLen;
	uint8* data;
};

// Tools > Memory searcher: finds guest addresses holding a value, narrows them down with
// follow-up filters, and keeps a table of stored addresses that can be watched, edited or
// frozen. The stored table is kept per title in <user data>/memorySearcher/<title id>.ini.
class MemorySearcherTool : public wxFrame
{
public:
	MemorySearcherTool(wxFrame* parent);
	virtual ~MemorySearcherTool();

	void OnTimerTick(wxTimerEvent& event);
	void OnClose(wxCloseEvent&);
	void OnSearch(wxCommandEvent&);
	void OnSearchFinished(wxCommandEvent& event);
	void OnUpdateGauge(wxSetGaugeValue& event);
	void OnFilter(wxCommandEvent& event);
	void OnResultListClick(wxMouseEvent& event);
	void OnEntryListRightClick(wxDataViewEvent& event);
	void OnPopupClick(wxCommandEvent& event);
	void OnItemEdited(wxDataViewEvent& event);

private:
	using ListType_t = std::vector<MEMPTR<void>>;

	void Reset();
	bool VerifySearchValue() const;
	void FillResultList();
	void RefreshResultList();
	void RefreshStashList();
	void SetSearchDataType();

	// runs job on the worker thread; its result replaces m_searchBuffer
	void RunOnWorker(std::function<ListType_t()> job);

	void Load();
	void Save();

	static bool IsAddressValid(uint32 addr);

	SearchDataType m_searchDataType = SearchDataType_None;
	wxComboBox* m_cbDataType;
	wxTextCtrl* m_textValue;
	wxButton *m_buttonStart, *m_buttonFilter;
	wxListView* m_listResults;
	wxDataViewListCtrl* m_listEntryTable;
	wxStaticText* m_textEntryTable;
	wxGauge* m_gauge;
	wxTimer* m_refresh_timer;

	ListType_t m_searchBuffer; // written by the worker under m_mutex
	std::mutex m_mutex;
	std::thread m_worker;

	std::atomic_bool m_running = true; // false once the window goes away; workers stop early
	std::atomic_bool m_search_running = false;

	bool m_clear_state = false; // results are shown: the Search button reads "Clear"
};
