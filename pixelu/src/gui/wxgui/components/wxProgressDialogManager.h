// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/thread.h>
#include "util/helpers/Semaphore.h"

// event types are file-local constants (one copy per translation unit)
wxDEFINE_EVENT(wxEVT_PROGRESSMGR_OPEN, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_PROGRESSMGR_CLOSE, wxThreadEvent);
wxDEFINE_EVENT(wxEVT_PROGRESSMGR_UPDATE, wxThreadEvent);

// Drives a wxGenericProgressDialog from a worker thread: every call is turned
// into an event that the GUI thread handles. Destroy() blocks until the dialog
// is gone. The dialog starts with the text "Please wait..." (the message passed
// to Create() is not shown); Update() sets the text and records cancel/skip.
class wxProgressDialogManager : public wxEvtHandler
{
public:
	wxProgressDialogManager(wxWindow* parent) : m_owner(parent)
	{
		Bind(wxEVT_PROGRESSMGR_OPEN, &wxProgressDialogManager::HandleOpen, this);
		Bind(wxEVT_PROGRESSMGR_CLOSE, &wxProgressDialogManager::HandleClose, this);
		Bind(wxEVT_PROGRESSMGR_UPDATE, &wxProgressDialogManager::HandleUpdate, this);
	}

	~wxProgressDialogManager()
	{
		if (m_dialog != nullptr)
			Destroy();
	}

	void Create(const wxString& title, const wxString& message, int maximum, int style = wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME)
	{
		(void)message;
		m_alive.increment();
		m_cancelled = false;
		m_skipped = false;
		wxThreadEvent evt(wxEVT_PROGRESSMGR_OPEN);
		evt.SetString(title);
		evt.SetInt(maximum);
		evt.SetExtraLong(style);
		wxQueueEvent(this, evt.Clone()); // Clone() deep-copies the string for the other thread
	}

	void Destroy()
	{
		wxThreadEvent evt(wxEVT_PROGRESSMGR_CLOSE);
		wxQueueEvent(this, evt.Clone());
		m_alive.waitUntilZero();
	}

	void Update(int value, const wxString& newmsg = wxEmptyString)
	{
		wxThreadEvent evt(wxEVT_PROGRESSMGR_UPDATE);
		evt.SetInt(value);
		evt.SetString(newmsg);
		wxQueueEvent(this, evt.Clone());
	}

	bool IsCancelled() const { return m_cancelled; }
	bool IsSkipped() const { return m_skipped; }
	bool IsCancelledOrSkipped() const { return m_cancelled || m_skipped; }

private:
	void HandleOpen(wxThreadEvent& evt)
	{
		if (m_dialog != nullptr)
		{
			// a second Create() without Destroy(): replace the dialog
			m_dialog->Destroy();
			m_dialog = nullptr;
			m_alive.decrement();
		}
		m_range = evt.GetInt();
		m_dialog = new wxGenericProgressDialog(evt.GetString(), "Please wait...", m_range, m_owner, (int)evt.GetExtraLong());
	}

	void HandleClose(wxThreadEvent&)
	{
		if (m_dialog == nullptr)
			return;
		m_dialog->Destroy();
		m_dialog = nullptr;
		m_alive.decrement();
	}

	void HandleUpdate(wxThreadEvent& evt)
	{
		if (m_dialog == nullptr)
			return;
		// the generic dialog misbehaves when it reaches its maximum before being destroyed
		const int pos = std::min(evt.GetInt(), m_range - 1);
		bool skipPressed = false;
		if (!m_dialog->Update(pos, evt.GetString(), &skipPressed))
			m_cancelled = true;
		if (skipPressed)
			m_skipped = true;
	}

	wxWindow* m_owner;
	wxGenericProgressDialog* m_dialog = nullptr;
	std::atomic<bool> m_cancelled{false};
	std::atomic<bool> m_skipped{false};
	int m_range = 0;
	CounterSemaphore m_alive; // non-zero while a dialog exists
};
