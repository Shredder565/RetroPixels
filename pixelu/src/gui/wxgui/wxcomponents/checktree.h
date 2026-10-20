// Original code for RetroPixels (PixelU). Not derived from Cemu.
#ifndef checktree_H_INCLUDED
#define checktree_H_INCLUDED

#include "wx/treectrl.h"
#include <map>

// The tree is compiled into the executable, so the export macro is plain WXEXPORT
// (kept so the event declarations below stay the same as before).
#define WXMAKINGDLL_CHECKTREE
#define WXDLLIMPEXP_CHECKTREE WXEXPORT

#include <wx/object.h>

// A wxTreeCtrl whose items can carry a check box, drawn through the tree's state
// images. An item's state image index is its check box state (see the enum);
// items without a check box have no state image (state < 0).
//
// Toggling a box with the mouse or the space key sends wxEVT_CHECKTREE_CHOICE
// with the item and GetExtraLong() == 1 when the box became checked.
// Keyboard focus entering the tree sends wxEVT_CHECKTREE_FOCUS.
// Sort() orders children with branches before leaves, then by text (case-insensitive).
class WXDLLIMPEXP_CHECKTREE wxCheckTree : public wxTreeCtrl
{
	wxDECLARE_ABSTRACT_CLASS(wxCheckTree);

public:
	wxCheckTree();
	wxCheckTree(wxWindow* parent, const wxWindowID id, const wxPoint& pos, const wxSize& size, long style);

	void Init();
	void Sort(const wxTreeItemId& node, bool recursive);

	int OnCompareItems(const wxTreeItemId& item1, const wxTreeItemId& item2) override;

	void SetFocusFromKbd() override;
	void SetItemTextColour(const wxTreeItemId& item, const wxColour& col) override;

	bool EnableCheckBox(const wxTreeItemId& item, bool enable = true);
	bool DisableCheckBox(const wxTreeItemId& item);
	void Check(const wxTreeItemId& item, bool state = true);
	void Uncheck(const wxTreeItemId& item);
	void MakeCheckable(const wxTreeItemId& item, bool state = false);
	bool IsCheckable(const wxTreeItemId& item);

	// check box states (= state image indices)
	enum
	{
		UNCHECKED,
		UNCHECKED_MOUSE_OVER,
		UNCHECKED_LEFT_DOWN,
		UNCHECKED_DISABLED,
		CHECKED,
		CHECKED_MOUSE_OVER,
		CHECKED_LEFT_DOWN,
		CHECKED_DISABLED
	};

private:
	void UpdateCheckBoxImages();

	// state transitions of one item's box
	void ShowNormal(const wxTreeItemId& item);
	void ShowHover(const wxTreeItemId& item, bool toggle);
	void ShowPressed(const wxTreeItemId& item);
	void ForgetPointer();

	void OnSelChanging(wxTreeEvent& event);
	void OnChar(wxKeyEvent& event);
	void OnKeyDown(wxKeyEvent& event);
	void OnKeyUp(wxKeyEvent& event);
	void OnEnterWindow(wxMouseEvent& event);
	void OnLeaveWindow(wxMouseEvent& event);
	void OnLeftDClick(wxMouseEvent& event);
	void OnLeftDown(wxMouseEvent& event);
	void OnLeftUp(wxMouseEvent& event);
	void OnMotion(wxMouseEvent& event);
	void OnWheel(wxMouseEvent& event);
	void OnFocusGained(wxFocusEvent& event);
	void OnFocusLost(wxFocusEvent& event);

	// text colour of each item as set by the owner; while a box is disabled its
	// text is greyed and the owner's colour is restored from here on enable
	std::map<wxTreeItemId, wxColour> m_ownerColours;

	bool m_dragEnteredPressed = false; // pointer came in with the left button already held
	wxTreeItemId m_hoverItem{};
	wxTreeItemId m_pressedItem{};
	wxTreeItemId m_keyItem{};
};

wxDECLARE_EXPORTED_EVENT(WXDLLIMPEXP_CHECKTREE, wxEVT_CHECKTREE_CHOICE, wxTreeEvent);
wxDECLARE_EXPORTED_EVENT(WXDLLIMPEXP_CHECKTREE, wxEVT_CHECKTREE_FOCUS, wxTreeEvent);

#endif // checktree_H_INCLUDED
