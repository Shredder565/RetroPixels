// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "DebugPPCThreadsWindow.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"

#include "wxgui/components/wxProgressDialogManager.h"

#include <cinttypes>
#include <helpers/wxHelpers.h>
#include <wx/listctrl.h>

namespace
{
	enum : int
	{
		ID_REFRESH = wxID_HIGHEST + 1,
		ID_AUTO_REFRESH,
		ID_CLOSE,
		ID_THREAD_LIST,

		ID_MENU_PRIO_UP_1,
		ID_MENU_PRIO_UP_5,
		ID_MENU_PRIO_DOWN_1,
		ID_MENU_PRIO_DOWN_5,
		ID_MENU_SUSPEND,
		ID_MENU_RESUME,
		ID_MENU_STACK_TRACE,
		ID_MENU_PROFILE,
	};

	enum Column : long
	{
		COL_ADDRESS,
		COL_ENTRY,
		COL_STACK,
		COL_PC,
		COL_LR,
		COL_STATE,
		COL_AFFINITY,
		COL_PRIORITY,
		COL_SLICE_START,
		COL_WAKE_TIME,
		COL_NAME,
		COL_GPR,
		COL_EXTRA,
	};

	struct ColumnSpec
	{
		const char* title;
		int width;
	};
	constexpr ColumnSpec kColumns[] = {
		{"Address", 75}, {"Entry", 75}, {"Stack", 145}, {"PC", 120}, {"LR", 75}, {"State", 90}, {"Affinity", 70},
		{"Priority", 80}, {"SliceStart", 110}, {"SumWakeTime", 110}, {"ThreadName", 180}, {"GPR", 180}, {"Extra info", 180},
	};

	// profiling stops by itself after this many samples (the bar is full)
	constexpr uint32 kProfileSampleTarget = 30000;

	const char* StateLabel(const OSThread_t* t)
	{
		if (t->suspendCounter != 0)
			return "SUSPENDED";
		switch (t->state)
		{
		case OSThread_t::THREAD_STATE::STATE_NONE: return "NONE";
		case OSThread_t::THREAD_STATE::STATE_READY: return "READY";
		case OSThread_t::THREAD_STATE::STATE_RUNNING: return "RUNNING";
		case OSThread_t::THREAD_STATE::STATE_WAITING: return "WAITING";
		case OSThread_t::THREAD_STATE::STATE_MORIBUND: return "MORIBUND";
		default: return "UNDEFINED";
		}
	}

	// cores 0,1,2 as three digits, e.g. "011"
	wxString CoreMask(uint8 mask)
	{
		return wxString::Format("%d%d%d", mask & 1, (mask >> 1) & 1, (mask >> 2) & 1);
	}

	bool IsThreadStillActive(OSThread_t* t)
	{
		__OSLockScheduler();
		const bool active = coreinit::__OSIsThreadActive(t);
		__OSUnlockScheduler();
		return active;
	}
}

DebugPPCThreadsWindow::DebugPPCThreadsWindow(wxFrame& parent)
	: wxFrame(&parent, wxID_ANY, _("PPC threads"), wxDefaultPosition, wxSize(930, 280), wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER)
{
	auto* layout = new wxBoxSizer(wxVERTICAL);

	m_list = new wxListView(this, ID_THREAD_LIST, wxPoint(0, 0), wxSize(930, 240), wxLC_REPORT);
	m_list->SetFont(wxFont(8, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Courier New"));
	long colIndex = 0;
	for (const ColumnSpec& c : kColumns)
	{
		m_list->InsertColumn(colIndex, c.title, wxLIST_FORMAT_LEFT, c.width);
		colIndex++;
	}
	layout->Add(m_list, 1, wxEXPAND | wxALL, 5);

	auto* bottom = new wxBoxSizer(wxHORIZONTAL);
	bottom->Add(new wxButton(this, ID_REFRESH, _("Refresh"), wxPoint(0, 0), wxSize(80, 26)), 0, wxALL, 5);
	m_autoRefresh = new wxCheckBox(this, ID_AUTO_REFRESH, _("Auto refresh"));
	m_autoRefresh->SetValue(true);
	bottom->Add(m_autoRefresh, 0, wxEXPAND | wxALL, 5);
	layout->Add(bottom, 0, wxEXPAND | wxALL, 5);

	Bind(wxEVT_BUTTON, &DebugPPCThreadsWindow::OnCloseButton, this, ID_CLOSE);
	Bind(wxEVT_BUTTON, &DebugPPCThreadsWindow::OnRefreshButton, this, ID_REFRESH);
	Bind(wxEVT_CLOSE_WINDOW, &DebugPPCThreadsWindow::OnClose, this);
	m_list->Bind(wxEVT_RIGHT_DOWN, &DebugPPCThreadsWindow::OnThreadListRightClick, this);

	SetSizer(layout);
	RefreshThreadList();

	m_refreshTimer = new wxTimer(this);
	Bind(wxEVT_TIMER, &DebugPPCThreadsWindow::OnTimer, this);
	m_refreshTimer->Start(250);
}

DebugPPCThreadsWindow::~DebugPPCThreadsWindow()
{
	m_refreshTimer->Stop();
}

void DebugPPCThreadsWindow::OnCloseButton(wxCommandEvent& /*event*/)
{
	Close();
}

void DebugPPCThreadsWindow::OnRefreshButton(wxCommandEvent& /*event*/)
{
	RefreshThreadList();
}

void DebugPPCThreadsWindow::OnClose(wxCloseEvent& /*event*/)
{
	Close();
}

void DebugPPCThreadsWindow::OnTimer(wxTimerEvent& /*event*/)
{
	if (m_autoRefresh->IsChecked())
		RefreshThreadList();
}

void DebugPPCThreadsWindow::FillRow(long row, uint32 threadAddr, OSThread_t* t)
{
	auto gpr = [t](int r) { return _swapEndianU32(t->context.gpr[r]); };

	m_list->SetItem(row, COL_ENTRY, wxString::Format("%08X", t->entrypoint.GetMPTR()));
	m_list->SetItem(row, COL_STACK, wxString::Format("%08X - %08X", t->stackEnd.GetMPTR(), t->stackBase.GetMPTR()));

	const uint32 pc = t->context.srr0;
	if (RPLStoredSymbol* sym = rplSymbolStorage_getByAddress(pc))
		m_list->SetItem(row, COL_PC, wxString::Format("%s (0x%08x)", (const char*)sym->symbolName, pc));
	else
		m_list->SetItem(row, COL_PC, wxString::Format("%08X", pc));
	m_list->SetItem(row, COL_LR, wxString::Format("%08X", _swapEndianU32(t->context.lr)));
	m_list->SetItem(row, COL_STATE, StateLabel(t));

	// configured affinity vs. the one the scheduler uses, flagged when they differ
	const uint8 wanted = t->attr & 7;
	const uint8 actual = t->context.affinity;
	if (wanted == actual)
		m_list->SetItem(row, COL_AFFINITY, CoreMask(wanted));
	else
		m_list->SetItem(row, COL_AFFINITY, "(!) " + CoreMask(wanted) + " real: " + CoreMask(actual));

	m_list->SetItem(row, COL_PRIORITY, wxString::Format("%d", (sint32)t->effectivePriority));
	m_list->SetItem(row, COL_SLICE_START, wxString::Format("%" PRIu64, (uint64)t->wakeUpTime));
	m_list->SetItem(row, COL_WAKE_TIME, wxString::Format("%" PRIu64, (uint64)t->totalCycles));
	m_list->SetItem(row, COL_NAME, t->threadName.IsNull() ? "NULL" : t->threadName.GetPtr());
	m_list->SetItem(row, COL_GPR, wxString::Format("r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x", gpr(3), gpr(4), gpr(5), gpr(6), gpr(7)));

	wxString extra;
	if (coreinit::OSMutex* m = t->waitingForMutex)
		extra = wxString::Format("Mutex 0x%08x (Held by thread 0x%08X Lock-Count: %d)", memory_getVirtualOffsetFromPointer(m), m->owner.GetMPTR(), (uint32)m->lockCount);
	if (t->requestFlags & OSThread_t::REQUEST_FLAG_CANCEL)
		extra += "[Cancel requested]";
	m_list->SetItem(row, COL_EXTRA, extra);
}

void DebugPPCThreadsWindow::RefreshThreadList()
{
	wxWindowUpdateLocker freeze(m_list);

	// remember selection (by thread address) and scroll position across the rebuild
	long keepThread = 0;
	const long selected = m_list->GetFirstSelected();
	if (selected != wxNOT_FOUND)
		keepThread = (long)m_list->GetItemData(selected);
	const int scroll = m_list->GetScrollPos(wxVERTICAL);

	m_list->DeleteAllItems();
	if (activeThreadCount > 0)
	{
		__OSLockScheduler();
		srwlock_activeThreadList.LockWrite();
		for (sint32 row = 0; row < activeThreadCount; row++)
		{
			const MPTR addr = activeThread[row];
			auto* t = (OSThread_t*)memory_getPointerFromVirtualOffset(addr);
			m_list->InsertItem(row, wxString::Format("%08X", addr));
			m_list->SetItemData(row, (long)addr);
			FillRow(row, addr, t);
			if (keepThread != 0 && keepThread == (long)addr)
			{
				m_list->Select(row);
				m_list->Focus(row);
			}
		}
		srwlock_activeThreadList.UnlockWrite();
		__OSUnlockScheduler();
	}
	m_list->SetScrollPos(wxVERTICAL, scroll, true);
}

void DebugPPCThreadsWindow::DumpStackTrace(OSThread_t* thread)
{
	cemuLog_log(LogType::Force, "Dumping stack trace for thread {0:08x} LR: {1:08x}", memory_getVirtualOffsetFromPointer(thread), _swapEndianU32(thread->context.lr));
	DebugLogStackTrace(thread, _swapEndianU32(thread->context.gpr[1]));
}

void DebugPPCThreadsWindow::PresentProfileResults(OSThread_t* thread, const std::unordered_map<VAddr, uint32>& samples)
{
	uint32 total = 0;
	for (const auto& [pc, count] : samples)
		total += count;
	cemuLog_log(LogType::Force, "--- Thread {:08x} profile results with {:} samples captured ---", MEMPTR<OSThread_t>(thread).GetMPTR(), total);
	cemuLog_log(LogType::Force, "Exclusive time, grouped by function:");

	// fold every sampled PC into the function (closest symbol) it belongs to
	std::vector<std::pair<VAddr, uint32>> perFunction;
	for (const auto& [pc, count] : samples)
	{
		RPLStoredSymbol* sym = rplSymbolStorage_getByClosestAddress(pc);
		const VAddr key = sym ? (VAddr)sym->address : pc;
		auto slot = std::find_if(perFunction.begin(), perFunction.end(), [key](const auto& e) { return e.first == key; });
		if (slot == perFunction.end())
			perFunction.emplace_back(key, count);
		else
			slot->second += count;
	}
	std::sort(perFunction.begin(), perFunction.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

	for (const auto& [addr, count] : perFunction)
	{
		if (count < 3)
			continue; // noise
		std::string label = "Unknown";
		if (RPLStoredSymbol* sym = rplSymbolStorage_getByClosestAddress(addr))
			label = fmt::format("{}.{}+0x{:x}", (const char*)sym->libName, (const char*)sym->symbolName, addr - sym->address);
		const double share = (double)(count * 100) / (double)total;
		cemuLog_log(LogType::Force, "[{:08x}] {:8.2f}% (Samples: {:5}) Symbol: {}", addr, share, count, label);
	}
}

void DebugPPCThreadsWindow::ProfileThreadWorker(OSThread_t* thread)
{
	wxProgressDialogManager progress(this);
	progress.Create(_("Profiling thread"), _("Capturing samples..."), 1000, wxPD_CAN_SKIP);

	std::unordered_map<VAddr, uint32> samples;
	uint32 taken = 0;
	for (;;)
	{
		// stop the thread, wait until it is off its core, read its PC, let it go
		coreinit::OSSuspendThread(thread);
		__OSLockScheduler();
		while (coreinit::OSIsThreadRunningNoLock(thread))
		{
			__OSUnlockScheduler();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			__OSLockScheduler();
		}
		const uint32 pc = thread->context.srr0;
		__OSUnlockScheduler();
		coreinit::OSResumeThread(thread);

		samples[pc]++;
		taken++;
		if (taken % 50 == 0)
		{
			wxString text = formatWxString(_("Capturing samples... ({:})\nResults will be written to log.txt\n"), taken);
			text.Append(taken < kProfileSampleTarget ? _("Click Skip button for early results with lower accuracy") : _("Click Skip button to finish"));
			progress.Update(taken * 1000 / kProfileSampleTarget, text);
			if (progress.IsCancelledOrSkipped())
				break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	PresentProfileResults(thread, samples);
	progress.Destroy();
}

void DebugPPCThreadsWindow::ProfileThread(OSThread_t* thread)
{
	std::thread(&DebugPPCThreadsWindow::ProfileThreadWorker, this, thread).detach();
}

void DebugPPCThreadsWindow::OnThreadListPopupClick(wxCommandEvent& evt)
{
	const MPTR addr = (MPTR)(size_t) static_cast<wxMenu*>(evt.GetEventObject())->GetClientData();
	auto* t = (OSThread_t*)memory_getPointerFromVirtualOffset(addr);
	if (!IsThreadStillActive(t))
		return;

	switch (evt.GetId())
	{
	case ID_MENU_PRIO_UP_5: t->basePriority = t->basePriority - 5; break;
	case ID_MENU_PRIO_UP_1: t->basePriority = t->basePriority - 1; break;
	case ID_MENU_PRIO_DOWN_5: t->basePriority = t->basePriority + 5; break;
	case ID_MENU_PRIO_DOWN_1: t->basePriority = t->basePriority + 1; break;
	case ID_MENU_SUSPEND: coreinit::OSSuspendThread(t); break;
	case ID_MENU_RESUME: coreinit::OSResumeThread(t); break;
	case ID_MENU_STACK_TRACE: DumpStackTrace(t); break;
	case ID_MENU_PROFILE: ProfileThread(t); break;
	default: break;
	}
	coreinit::__OSUpdateThreadEffectivePriority(t);
	RefreshThreadList();
}

void DebugPPCThreadsWindow::OnThreadListRightClick(wxMouseEvent& event)
{
	int hitFlags = 0;
	const long row = m_list->HitTest(event.GetPosition(), hitFlags);
	if (row == wxNOT_FOUND)
		return;

	// make the clicked row the only selected one
	m_list->Focus(row);
	const long previous = m_list->GetFirstSelected();
	if (previous != wxNOT_FOUND)
		m_list->Select(previous, false);
	m_list->Select(row);

	const MPTR addr = (MPTR)m_list->GetItemData(row);
	if (!IsThreadStillActive(MEMPTR<OSThread_t>(addr).GetPtr()))
		return;

	wxMenu menu;
	menu.SetClientData((void*)(size_t)addr);
	menu.Append(ID_MENU_PRIO_UP_5, _("Boost priority (-5)"));
	menu.Append(ID_MENU_PRIO_UP_1, _("Boost priority (-1)"));
	menu.AppendSeparator();
	menu.Append(ID_MENU_PRIO_DOWN_5, _("Decrease priority (+5)"));
	menu.Append(ID_MENU_PRIO_DOWN_1, _("Decrease priority (+1)"));
	menu.AppendSeparator();
	menu.Append(ID_MENU_RESUME, _("Resume"));
	menu.Append(ID_MENU_SUSPEND, _("Suspend"));
	menu.AppendSeparator();
	menu.Append(ID_MENU_STACK_TRACE, _("Write stack trace to log"));
	menu.Append(ID_MENU_PROFILE, _("Profile thread"));
	menu.Bind(wxEVT_MENU, &DebugPPCThreadsWindow::OnThreadListPopupClick, this);
	PopupMenu(&menu);
}

void DebugPPCThreadsWindow::Close()
{
	Destroy();
}
