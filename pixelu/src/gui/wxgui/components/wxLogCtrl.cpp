// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/components/wxLogCtrl.h"

#include <boost/algorithm/string.hpp>

namespace
{
	// posted by the rebuild worker when the visible list is ready
	wxDEFINE_EVENT(wxEVT_LOGCTRL_REBUILT, wxThreadEvent);
}

wxLogCtrl::wxLogCtrl(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, bool alternateRowColours)
	: TextList(parent, id, pos, size, style), m_pollTimer(this), m_stripes(alternateRowColours)
{
	Bind(wxEVT_TIMER, &wxLogCtrl::OnPollTimer, this, m_pollTimer.GetId());
	Bind(wxEVT_LOGCTRL_REBUILT, &wxLogCtrl::OnRebuildDone, this);
	m_pollTimer.Start(250);
}

wxLogCtrl::~wxLogCtrl()
{
	m_pollTimer.Stop();
	if (m_rebuildThread.joinable())
		m_rebuildThread.join();
}

void wxLogCtrl::SetActiveFilter(const std::string& active_filter)
{
	if (active_filter == m_filterText)
		return;
	m_filterText = active_filter;
	RestartRebuild();
}

const std::string& wxLogCtrl::GetActiveFilter() const
{
	return m_filterText;
}

void wxLogCtrl::SetFilterMessage(bool state)
{
	if (state == m_searchMessages)
		return;
	m_searchMessages = state;
	RestartRebuild();
}

bool wxLogCtrl::GetFilterMessage() const
{
	return m_searchMessages;
}

bool wxLogCtrl::Matches(const Entry& e) const
{
	if (m_filterText.empty() || e.filter == m_filterText)
		return true;
	return m_searchMessages && boost::icontains(e.message.ToStdString(), m_filterText);
}

void wxLogCtrl::RestartRebuild()
{
	if (m_rebuildThread.joinable())
		m_rebuildThread.join();
	m_rebuildThread = std::thread(&wxLogCtrl::RebuildVisible, this);
}

void wxLogCtrl::RebuildVisible()
{
	{
		std::scoped_lock both(m_entriesLock, m_visibleLock);
		m_visible.clear();
		for (const Entry& e : m_entries)
		{
			if (Matches(e))
				m_visible.push_back(&e);
		}
	}
	wxQueueEvent(this, new wxThreadEvent(wxEVT_LOGCTRL_REBUILT));
}

void wxLogCtrl::PushEntry(const wxString& filter, const wxString& message)
{
	size_t visibleCount = 0;
	{
		std::scoped_lock both(m_entriesLock, m_visibleLock);
		const Entry& added = m_entries.emplace_back(Entry{filter, message});
		if (!Matches(added))
			return;
		m_visible.push_back(&added);
		visibleCount = m_visible.size();
	}
	// only lines on the first screen need an immediate repaint, the timer handles the rest
	if (visibleCount <= m_elements_visible)
		RefreshLine((uint32)(visibleCount - 1));
}

wxColour wxLogCtrl::StripeColour()
{
	if (!m_stripeColour.IsOk())
	{
		// slightly darker on light backgrounds, slightly lighter on dark ones
		const wxColour bg = GetBackgroundColour();
		m_stripeColour = bg.ChangeLightness(bg.GetRGB() > 0x808080 ? 97 : 110);
	}
	return m_stripeColour;
}

void wxLogCtrl::OnDraw(wxDC& dc, sint32 start, sint32 count, const wxPoint& start_position)
{
	std::scoped_lock lock(m_visibleLock);
	if (start < 0 || (size_t)start >= m_visible.size())
		return;
	wxPoint at = start_position;
	const wxColour textColour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	for (sint32 line = start; line <= start + count && (size_t)line < m_visible.size(); line++)
	{
		const bool striped = m_stripes && (line % 2) != 0;
		DrawLineBackground(dc, at, striped ? StripeColour() : GetBackgroundColour());
		dc.SetTextForeground(textColour);
		dc.DrawText(m_visible[line]->message, at);
		NextLine(at, &start_position);
	}
}

void wxLogCtrl::OnPollTimer(wxTimerEvent& /*event*/)
{
	size_t visibleCount;
	{
		std::scoped_lock lock(m_visibleLock);
		visibleCount = m_visible.size();
	}
	if (visibleCount == m_element_count)
		return;
	SetElementCount(visibleCount);
	if (m_scrolled_to_end)
	{
		Scroll(0, (int)visibleCount);
		RefreshControl();
	}
}

void wxLogCtrl::OnRebuildDone(wxThreadEvent& /*event*/)
{
	size_t visibleCount;
	{
		std::scoped_lock lock(m_visibleLock);
		visibleCount = m_visible.size();
	}
	SetElementCount(visibleCount);
	RefreshControl();
}
