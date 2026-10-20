// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxcomponents/checktree.h"

#include <wx/dcmemory.h>
#include <wx/icon.h>
#include <wx/bmpbndl.h>
#include <wx/renderer.h>

wxDEFINE_EVENT(wxEVT_CHECKTREE_FOCUS, wxTreeEvent);
wxDEFINE_EVENT(wxEVT_CHECKTREE_CHOICE, wxTreeEvent);

wxIMPLEMENT_CLASS(wxCheckTree, wxTreeCtrl);

namespace
{
	constexpr bool HitsBox(int hitFlags) { return (hitFlags & wxTREE_HITTEST_ONITEMSTATEICON) != 0; }
	constexpr bool HitsLabel(int hitFlags) { return (hitFlags & wxTREE_HITTEST_ONITEMLABEL) != 0; }

	// enabled boxes only: the three visual variants of each value
	constexpr bool IsLiveUnchecked(int s) { return s >= wxCheckTree::UNCHECKED && s < wxCheckTree::UNCHECKED_DISABLED; }
	constexpr bool IsLiveChecked(int s) { return s >= wxCheckTree::CHECKED && s < wxCheckTree::CHECKED_DISABLED; }
	constexpr bool IsBoxState(int s) { return s >= wxCheckTree::UNCHECKED && s <= wxCheckTree::CHECKED_DISABLED; }
	constexpr bool IsDisabledState(int s) { return s == wxCheckTree::UNCHECKED_DISABLED || s == wxCheckTree::CHECKED_DISABLED; }

	const wxColour kDisabledText(161, 161, 146);
}

wxCheckTree::wxCheckTree()
{
	Init();
}

wxCheckTree::wxCheckTree(wxWindow* parent, const wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
	: wxTreeCtrl(parent, id, pos, size, style)
{
	Init();
}

void wxCheckTree::Init()
{
	UpdateCheckBoxImages();
	// the box bitmaps depend on the theme and the DPI, redraw them when either changes
	Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& evt) {
		evt.Skip();
		CallAfter(&wxCheckTree::UpdateCheckBoxImages);
	});
	Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& evt) {
		evt.Skip();
		CallAfter(&wxCheckTree::UpdateCheckBoxImages);
	});

	Bind(wxEVT_TREE_SEL_CHANGING, &wxCheckTree::OnSelChanging, this);
	Bind(wxEVT_CHAR, &wxCheckTree::OnChar, this);
	Bind(wxEVT_KEY_DOWN, &wxCheckTree::OnKeyDown, this);
	Bind(wxEVT_KEY_UP, &wxCheckTree::OnKeyUp, this);
	Bind(wxEVT_ENTER_WINDOW, &wxCheckTree::OnEnterWindow, this);
	Bind(wxEVT_LEAVE_WINDOW, &wxCheckTree::OnLeaveWindow, this);
	Bind(wxEVT_LEFT_DCLICK, &wxCheckTree::OnLeftDClick, this);
	Bind(wxEVT_LEFT_DOWN, &wxCheckTree::OnLeftDown, this);
	Bind(wxEVT_LEFT_UP, &wxCheckTree::OnLeftUp, this);
	Bind(wxEVT_MOTION, &wxCheckTree::OnMotion, this);
	Bind(wxEVT_MOUSEWHEEL, &wxCheckTree::OnWheel, this);
	Bind(wxEVT_SET_FOCUS, &wxCheckTree::OnFocusGained, this);
	Bind(wxEVT_KILL_FOCUS, &wxCheckTree::OnFocusLost, this);
}

void wxCheckTree::UpdateCheckBoxImages()
{
	// one native check box rendering per state, in enum order
	// unchecked states first, then the same four with wxCONTROL_CHECKED added
	static constexpr int kRenderFlags[] = {
		wxCONTROL_NONE, wxCONTROL_CURRENT, wxCONTROL_CURRENT | wxCONTROL_PRESSED, wxCONTROL_DISABLED,
		wxCONTROL_CHECKED, wxCONTROL_CHECKED | wxCONTROL_CURRENT, wxCONTROL_CHECKED | wxCONTROL_CURRENT | wxCONTROL_PRESSED, wxCONTROL_CHECKED | wxCONTROL_DISABLED,
	};
	wxRendererNative& renderer = wxRendererNative::Get();
	const wxSize boxSize = renderer.GetCheckBoxSize(this);
	wxVector<wxBitmapBundle> images;
	for (int flags : kRenderFlags)
	{
		wxBitmap bmp;
#ifdef __WXOSX__
		bmp.CreateWithDIPSize(boxSize, GetDPIScaleFactor(), 32);
		bmp.UseAlpha();
#else
		bmp.Create(boxSize.x, boxSize.y);
#endif
		{
			wxMemoryDC dc(bmp);
#ifdef __WXOSX__
			dc.SetBackground(wxBrush(wxColour(0, 0, 0, 0)));
#else
			dc.SetBackground(wxBrush(GetBackgroundColour()));
#endif
			dc.Clear();
			renderer.DrawCheckBox(this, dc, wxRect(wxPoint(0, 0), boxSize), flags);
			dc.SelectObject(wxNullBitmap);
		}
		images.push_back(wxBitmapBundle::FromBitmap(bmp));
	}
	SetStateImages(images);
	Refresh();
}

void wxCheckTree::Sort(const wxTreeItemId& node, bool recursive)
{
	if (recursive)
	{
		wxTreeItemIdValue cookie;
		wxTreeItemId child = GetFirstChild(node, cookie);
		while (child.IsOk())
		{
			Sort(child, true);
			child = GetNextChild(child, cookie);
		}
	}
	if (GetChildrenCount(node, false) != 0)
		SortChildren(node);
}

int wxCheckTree::OnCompareItems(const wxTreeItemId& item1, const wxTreeItemId& item2)
{
	const bool leaf1 = GetChildrenCount(item1, false) == 0;
	const bool leaf2 = GetChildrenCount(item2, false) == 0;
	if (leaf1 != leaf2)
		return leaf1 ? 1 : -1; // folders first
	return GetItemText(item1).Lower().compare(GetItemText(item2).Lower());
}

void wxCheckTree::SetItemTextColour(const wxTreeItemId& item, const wxColour& col)
{
	m_ownerColours[item] = col;
	wxTreeCtrl::SetItemTextColour(item, col);
}

bool wxCheckTree::EnableCheckBox(const wxTreeItemId& item, bool enable)
{
	if (!item.IsOk())
		return false;
	const int s = GetItemState(item);
	if (!IsBoxState(s))
		return false;

	if (enable)
	{
		if (s == UNCHECKED_DISABLED)
			SetItemState(item, UNCHECKED);
		else if (s == CHECKED_DISABLED)
			SetItemState(item, CHECKED);
		auto saved = m_ownerColours.find(item);
		if (saved != m_ownerColours.end())
		{
			const wxColour original = saved->second;
			wxTreeCtrl::SetItemTextColour(item, original);
			m_ownerColours.erase(item);
		}
		return true;
	}

	if (IsDisabledState(s))
		return true; // already disabled, the saved colour must not be overwritten with the grey one

	SetItemState(item, IsLiveUnchecked(s) ? UNCHECKED_DISABLED : CHECKED_DISABLED);
	const wxColour original = GetItemTextColour(item);
	wxTreeCtrl::SetItemTextColour(item, kDisabledText);
	m_ownerColours[item] = original;
	return true;
}

bool wxCheckTree::DisableCheckBox(const wxTreeItemId& item)
{
	return EnableCheckBox(item, false);
}

void wxCheckTree::MakeCheckable(const wxTreeItemId& item, bool state)
{
	if (!item.IsOk())
		return;
	if (!IsBoxState(GetItemState(item)))
		SetItemState(item, state ? CHECKED : UNCHECKED);
}

bool wxCheckTree::IsCheckable(const wxTreeItemId& item)
{
	return item.IsOk() && IsBoxState(GetItemState(item));
}

void wxCheckTree::Check(const wxTreeItemId& item, bool state)
{
	if (!item.IsOk())
		return;
	const int s = GetItemState(item);
	if (!IsBoxState(s))
		return;
	const bool disabled = IsDisabledState(s);
	int wanted;
	if (state)
		wanted = disabled ? CHECKED_DISABLED : CHECKED;
	else
		wanted = disabled ? UNCHECKED_DISABLED : UNCHECKED;
	if (wanted != s)
		SetItemState(item, wanted);
}

void wxCheckTree::Uncheck(const wxTreeItemId& item)
{
	Check(item, false);
}

// --- box visuals -------------------------------------------------------------

void wxCheckTree::ShowNormal(const wxTreeItemId& item)
{
	if (!item.IsOk())
		return;
	const int s = GetItemState(item);
	if (IsLiveUnchecked(s))
		SetItemState(item, UNCHECKED);
	else if (IsLiveChecked(s))
		SetItemState(item, CHECKED);
}

void wxCheckTree::ShowPressed(const wxTreeItemId& item)
{
	if (!item.IsOk())
		return;
	const int s = GetItemState(item);
	if (IsLiveUnchecked(s))
		SetItemState(item, UNCHECKED_LEFT_DOWN);
	else if (IsLiveChecked(s))
		SetItemState(item, CHECKED_LEFT_DOWN);
}

// Shows the hover look; with toggle the value flips first and the owner is told
// (GetExtraLong() == 1 means the box is now checked; a disabled box reports 0).
void wxCheckTree::ShowHover(const wxTreeItemId& item, bool toggle)
{
	if (!item.IsOk())
		return;
	const int s = GetItemState(item);
	if (s < 0)
		return;
	const bool wasUnchecked = IsLiveUnchecked(s);
	if (wasUnchecked)
		SetItemState(item, toggle ? CHECKED_MOUSE_OVER : UNCHECKED_MOUSE_OVER);
	else if (IsLiveChecked(s))
		SetItemState(item, toggle ? UNCHECKED_MOUSE_OVER : CHECKED_MOUSE_OVER);
	if (!toggle)
		return;
	wxTreeEvent notify(wxEVT_CHECKTREE_CHOICE, this, item);
	notify.SetExtraLong(wasUnchecked ? 1 : 0);
	ProcessWindowEvent(notify);
}

void wxCheckTree::ForgetPointer()
{
	ShowNormal(m_pressedItem);
	ShowNormal(m_hoverItem);
	m_pressedItem = wxTreeItemId();
	m_hoverItem = wxTreeItemId();
}

// --- keyboard ------------------------------------------------------------------

void wxCheckTree::OnSelChanging(wxTreeEvent& event)
{
	const wxTreeItemId next = event.GetItem();
	ShowNormal(m_keyItem);
	ShowHover(next, false);
	m_keyItem = next;
	event.Skip();
}

void wxCheckTree::OnChar(wxKeyEvent& event)
{
	if (GetSelection().IsOk() || GetCount() == 0)
	{
		event.Skip();
		return;
	}
	// nothing selected yet: the first key press only selects the first item
	wxTreeItemId first = GetRootItem();
	if (HasFlag(wxTR_HIDE_ROOT))
	{
		wxTreeItemIdValue cookie;
		first = GetFirstChild(first, cookie);
	}
	SelectItem(first);
	m_keyItem = first;
}

void wxCheckTree::OnKeyDown(wxKeyEvent& event)
{
	if (event.GetKeyCode() == WXK_SPACE)
	{
		m_keyItem = GetSelection();
		ShowPressed(m_keyItem);
	}
	event.Skip();
}

void wxCheckTree::OnKeyUp(wxKeyEvent& event)
{
	const int key = event.GetKeyCode();
	if (key == WXK_SPACE)
	{
		ShowHover(m_keyItem, true);
	}
	else if (key == WXK_ESCAPE)
	{
		ShowNormal(m_keyItem);
		m_keyItem = wxTreeItemId();
		Unselect();
	}
	event.Skip();
}

// --- mouse ---------------------------------------------------------------------

void wxCheckTree::OnEnterWindow(wxMouseEvent& event)
{
	if (event.LeftIsDown())
		m_dragEnteredPressed = true;
}

void wxCheckTree::OnLeaveWindow(wxMouseEvent& /*event*/)
{
	ShowNormal(m_hoverItem);
	ShowNormal(m_pressedItem);
	m_hoverItem = wxTreeItemId();
	m_pressedItem = wxTreeItemId();
}

void wxCheckTree::OnLeftDClick(wxMouseEvent& event)
{
	// a double click counts as one more click, except on the expand button
	int hit = 0;
	HitTest(event.GetPosition(), hit);
	if (hit & wxTREE_HITTEST_ONITEMBUTTON)
		return;
	OnLeftDown(event);
	OnLeftUp(event);
}

void wxCheckTree::OnLeftDown(wxMouseEvent& event)
{
	int hit = 0;
	const wxTreeItemId item = HitTest(event.GetPosition(), hit);
	if (!item.IsOk())
		return;
	if (GetItemState(item) >= 0 && HitsBox(hit))
	{
		m_pressedItem = item;
		ShowPressed(item);
	}
	else if (HitsLabel(hit))
	{
		event.Skip();
	}
}

void wxCheckTree::OnLeftUp(wxMouseEvent& event)
{
	SetFocus();
	int hit = 0;
	const wxTreeItemId item = HitTest(event.GetPosition(), hit);
	if (!item.IsOk())
		return;
	const bool onBox = GetItemState(item) >= 0 && HitsBox(hit);

	if (m_dragEnteredPressed)
	{
		// the press started outside the tree: no toggle, just hover
		m_dragEnteredPressed = false;
		if (onBox)
		{
			ShowHover(item, false);
			m_hoverItem = item;
		}
		return;
	}

	if ((hit & wxTREE_HITTEST_ONITEMBUTTON) && ItemHasChildren(item))
	{
		if (IsExpanded(item))
			Collapse(item);
		else
			Expand(item);
	}
	else if (onBox)
	{
		if (item == m_pressedItem)
		{
			ShowHover(item, true); // press and release on the same box toggles it
		}
		else
		{
			ShowNormal(m_pressedItem);
			ShowHover(item, false);
		}
		m_pressedItem = wxTreeItemId();
		m_hoverItem = item;
	}
	else if (HitsLabel(hit))
	{
		event.Skip();
	}
	else
	{
		ForgetPointer();
	}
}

void wxCheckTree::OnMotion(wxMouseEvent& event)
{
	if (m_dragEnteredPressed)
		return; // wait for the button to be released

	int hit = 0;
	const wxTreeItemId item = HitTest(event.GetPosition(), hit);
	if (!item.IsOk())
	{
		ShowNormal(m_hoverItem);
		m_hoverItem = wxTreeItemId();
		return;
	}

	if (event.LeftIsDown() && m_pressedItem.IsOk())
	{
		// like a normal check box: pressed look only while over the box that was pressed
		if (item != m_pressedItem)
		{
			ShowHover(m_pressedItem, false);
			return;
		}
		const int s = GetItemState(m_pressedItem);
		if (s != UNCHECKED_LEFT_DOWN && s != CHECKED_LEFT_DOWN)
			ShowPressed(m_pressedItem);
		return;
	}

	if (GetItemState(item) < 0 || !HitsBox(hit))
	{
		ShowNormal(m_hoverItem);
		m_hoverItem = wxTreeItemId();
	}
	else if (item != m_hoverItem)
	{
		ShowNormal(m_hoverItem);
		ShowHover(item, false);
		m_hoverItem = item;
	}
}

void wxCheckTree::OnWheel(wxMouseEvent& event)
{
	event.Skip();
}

// --- focus ---------------------------------------------------------------------

void wxCheckTree::OnFocusGained(wxFocusEvent& /*event*/)
{
	// not skipped: the default handler would highlight the last selection, items
	// should only light up through keyboard navigation
}

void wxCheckTree::OnFocusLost(wxFocusEvent& event)
{
	ShowNormal(m_keyItem);
	Unselect();
	event.Skip();
}

void wxCheckTree::SetFocusFromKbd()
{
	if (m_keyItem.IsOk())
		SelectItem(m_keyItem);
	wxTreeEvent notify(wxEVT_CHECKTREE_FOCUS, this, wxTreeItemId());
	ProcessWindowEvent(notify);
	wxWindow::SetFocusFromKbd();
}
