// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/wx.h>

class wxListCtrl;

// Debug window listing the GPU texture cache: one row per texture (sorted by
// physical address), optionally followed by one row per view of it.
class TextureRelationViewerWindow : public wxFrame
{
public:
	TextureRelationViewerWindow(wxFrame& parent);
	~TextureRelationViewerWindow();

	void OnCloseButton(wxCommandEvent& event);
	void OnRefreshButton(wxCommandEvent& event);
	void OnCheckbox(wxCommandEvent& event);
	void OnClose(wxCloseEvent& event);
	void RefreshTextureList();
	void OnTextureListRightClick(wxMouseEvent& event);

	void Close();

private:
	void AddTextureRow(long row, const struct LatteTextureInformation& tex);
	void AddViewRow(long row, const struct LatteTextureViewInformation& view);

	wxListCtrl* m_list;
	bool m_onlyRecent = false;
	bool m_withViews = true;
};

// opens the window unless one is already open
void openTextureViewer(wxFrame& parentFrame);
