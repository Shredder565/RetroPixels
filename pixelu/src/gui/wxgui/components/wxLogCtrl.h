// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "TextList.h"

#include <deque>

// Log view: keeps every pushed (filter, message) entry and shows those matching
// the active filter. An entry matches when no filter is set, when its filter
// equals the active one, or (with "filter messages" on) when its message
// contains the filter text, case-insensitively. Changing the filter rebuilds
// the visible list on a worker thread. The view follows new entries while it is
// scrolled to the end.
class wxLogCtrl : public TextList
{
public:
	wxLogCtrl(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, bool alternateRowColours = false);
	~wxLogCtrl();

	void SetActiveFilter(const std::string& active_filter);
	[[nodiscard]] const std::string& GetActiveFilter() const;
	void SetFilterMessage(bool state);
	[[nodiscard]] bool GetFilterMessage() const;

	void PushEntry(const wxString& filter, const wxString& message);

protected:
	void OnDraw(wxDC& dc, sint32 start, sint32 count, const wxPoint& start_position);

private:
	struct Entry
	{
		wxString filter;
		wxString message;
	};

	bool Matches(const Entry& e) const;
	void RestartRebuild();
	void RebuildVisible(); // worker thread
	void OnPollTimer(wxTimerEvent& event);
	void OnRebuildDone(wxThreadEvent& event);
	wxColour StripeColour();

	wxTimer m_pollTimer;
	bool m_stripes = false;
	wxColour m_stripeColour;

	std::string m_filterText;
	bool m_searchMessages = false;
	std::thread m_rebuildThread;

	std::mutex m_entriesLock;  // guards m_entries
	std::mutex m_visibleLock;  // guards m_visible
	std::deque<Entry> m_entries; // element addresses stay valid while appending
	std::vector<const Entry*> m_visible;
};
