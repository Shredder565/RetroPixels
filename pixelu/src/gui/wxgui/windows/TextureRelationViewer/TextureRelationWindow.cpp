// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxHelper.h"
#include "TextureRelationWindow.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

namespace
{
	enum : int
	{
		ID_REFRESH = wxID_HIGHEST + 1,
		ID_CLOSE,
		ID_TEXTURE_LIST,
		ID_ONLY_ACTIVE,
		ID_SHOW_VIEWS,
	};

	enum Column : long
	{
		COL_TYPE,
		COL_PHYS,
		COL_MIP_PHYS,
		COL_DIM,
		COL_SIZE,
		COL_FORMAT,
		COL_PITCH,
		COL_TILEMODE,
		COL_SLICES,
		COL_MIPS,
		COL_LAST_ACCESS,
		COL_OVERWRITE,
		COL_COUNT
	};

	struct ColumnSpec
	{
		const char* title;
		int width;
	};
	constexpr ColumnSpec kColumns[COL_COUNT] = {
		{"Type", 85}, {"PhysAddr", 80}, {"MipPAddr", 80}, {"Dim", 80}, {"Resolution", 110}, {"Format", 70},
		{"Pitch", 80}, {"Tilemode", 80}, {"SliceRange", 90}, {"MipRange", 90}, {"Last access", 90}, {"OverwriteRes", 110},
	};

	// textures not touched for longer than this are hidden by "Show only active"
	constexpr uint32 kActiveWindowMs = 3000;

	// the texture rows and the view rows spell the array MSAA dimension differently
	wxString DimLabel(Latte::E_DIM dim, bool isView)
	{
		switch (dim)
		{
		case Latte::E_DIM::DIM_1D: return "1D";
		case Latte::E_DIM::DIM_2D: return "2D";
		case Latte::E_DIM::DIM_2D_ARRAY: return "2D_ARRAY";
		case Latte::E_DIM::DIM_3D: return "3D";
		case Latte::E_DIM::DIM_CUBEMAP: return "CUBEMAP";
		case Latte::E_DIM::DIM_2D_MSAA: return "2D_MSAA";
		case Latte::E_DIM::DIM_2D_ARRAY_MSAA: return isView ? "2D_ARRAY_MSAA" : "2D_MS_ARRAY";
		default: return "UKN";
		}
	}

	wxString SizeLabel(sint32 w, sint32 h, sint32 d, bool forceDepth)
	{
		if (d == 1 && !forceDepth)
			return wxString::Format("%dx%d", w, h);
		return wxString::Format("%dx%dx%d", w, h, d);
	}

	bool s_viewerOpen = false;
}

void openTextureViewer(wxFrame& parentFrame)
{
	if (s_viewerOpen)
		return;
	auto* window = new TextureRelationViewerWindow(parentFrame);
	window->Show(true);
}

TextureRelationViewerWindow::TextureRelationViewerWindow(wxFrame& parent)
	: wxFrame(&parent, wxID_ANY, _("Texture cache"), wxDefaultPosition, wxSize(1000, 480), wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER)
{
	s_viewerOpen = true;

	auto* pane = new wxPanel(this);
	auto* layout = new wxBoxSizer(wxVERTICAL);

	m_list = new wxListCtrl(pane, ID_TEXTURE_LIST, wxPoint(0, 0), wxSize(1008, 440), wxLC_REPORT);
	m_list->SetFont(wxFont(8, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, "Courier New"));
	for (long c = 0; c < COL_COUNT; c++)
		m_list->InsertColumn(c, kColumns[c].title, wxLIST_FORMAT_LEFT, kColumns[c].width);
	layout->Add(m_list, 1, wxEXPAND | wxBOTTOM, 0);

	auto* bottom = new wxBoxSizer(wxHORIZONTAL);
	const int rowFlags = wxALIGN_CENTER_VERTICAL | wxBOTTOM | wxTOP | wxLEFT;
	bottom->Add(new wxButton(pane, ID_REFRESH, _("Refresh")), 0, rowFlags, 10);
	bottom->Add(new wxCheckBox(pane, ID_ONLY_ACTIVE, _("Show only active")), 0, rowFlags, 10);
	auto* showViews = new wxCheckBox(pane, ID_SHOW_VIEWS, _("Show views"));
	showViews->SetValue(true);
	bottom->Add(showViews, 0, rowFlags, 10);
	layout->Add(bottom, 0, wxALIGN_LEFT);
	pane->SetSizer(layout);

	Bind(wxEVT_BUTTON, &TextureRelationViewerWindow::OnCloseButton, this, ID_CLOSE);
	Bind(wxEVT_BUTTON, &TextureRelationViewerWindow::OnRefreshButton, this, ID_REFRESH);
	Bind(wxEVT_CHECKBOX, &TextureRelationViewerWindow::OnCheckbox, this, ID_ONLY_ACTIVE);
	Bind(wxEVT_CHECKBOX, &TextureRelationViewerWindow::OnCheckbox, this, ID_SHOW_VIEWS);
	Bind(wxEVT_CLOSE_WINDOW, &TextureRelationViewerWindow::OnClose, this);
	m_list->Bind(wxEVT_RIGHT_DOWN, &TextureRelationViewerWindow::OnTextureListRightClick, this);

	RefreshTextureList();
}

TextureRelationViewerWindow::~TextureRelationViewerWindow()
{
	s_viewerOpen = false;
}

void TextureRelationViewerWindow::OnCloseButton(wxCommandEvent& /*event*/)
{
	Close();
}

void TextureRelationViewerWindow::OnRefreshButton(wxCommandEvent& /*event*/)
{
	RefreshTextureList();
}

void TextureRelationViewerWindow::OnCheckbox(wxCommandEvent& event)
{
	switch (event.GetId())
	{
	case ID_ONLY_ACTIVE: m_onlyRecent = event.IsChecked(); break;
	case ID_SHOW_VIEWS: m_withViews = event.IsChecked(); break;
	default: return;
	}
	RefreshTextureList();
}

void TextureRelationViewerWindow::OnClose(wxCloseEvent& /*event*/)
{
	Close();
}

void TextureRelationViewerWindow::AddTextureRow(long row, const LatteTextureInformation& tex)
{
	wxString type = tex.isUpdatedOnGPU ? "TEXTURE*" : "TEXTURE";
	if (tex.alternativeViewCount > 0)
		type << wxString::Format("(%d)", (sint32)tex.alternativeViewCount + 1);
	wxListItem item;
	item.SetId(row);
	item.SetText(type);
	item.SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
	m_list->InsertItem(item);

	m_list->SetItem(row, COL_PHYS, wxString::Format("%08X", tex.physAddress));
	m_list->SetItem(row, COL_MIP_PHYS, wxString::Format("%08X", tex.physMipAddress));
	m_list->SetItem(row, COL_DIM, DimLabel(tex.dim, false));
	m_list->SetItem(row, COL_SIZE, SizeLabel(tex.width, tex.height, tex.depth, false));
	m_list->SetItem(row, COL_FORMAT, wxString::Format(tex.isDepth ? "%04x(d)" : "%04x", (uint32)tex.format));
	m_list->SetItem(row, COL_PITCH, wxString::Format("%d", tex.pitch));
	m_list->SetItem(row, COL_TILEMODE, wxString::Format("%d", (int)tex.tileMode));
	m_list->SetItem(row, COL_SLICES, "");
	m_list->SetItem(row, COL_MIPS, tex.mipLevels == 1 ? wxString("1 mip") : wxString::Format("%d mips", tex.mipLevels));
	// tick values are milliseconds; the column has always shown them rounded to seconds with a "us" suffix
	m_list->SetItem(row, COL_LAST_ACCESS, wxString::Format("%uus", (GetTickCount() - tex.lastAccessTick + 499) / 1000));
	wxString overwrite;
	if (tex.overwriteInfo.hasResolutionOverwrite)
		overwrite = SizeLabel(tex.overwriteInfo.width, tex.overwriteInfo.height, tex.overwriteInfo.depth, tex.depth != 1);
	m_list->SetItem(row, COL_OVERWRITE, overwrite);
}

void TextureRelationViewerWindow::AddViewRow(long row, const LatteTextureViewInformation& view)
{
	wxListItem item;
	item.SetId(row);
	item.SetText("> VIEW");
	item.SetBackgroundColour(wxHelper::CalculateAccentColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
	m_list->InsertItem(item);

	m_list->SetItem(row, COL_PHYS, "");
	m_list->SetItem(row, COL_MIP_PHYS, "");
	m_list->SetItem(row, COL_DIM, DimLabel(view.dim, true));
	m_list->SetItem(row, COL_SIZE, "");
	m_list->SetItem(row, COL_FORMAT, wxString::Format("%04x", (uint32)view.format));
	m_list->SetItem(row, COL_PITCH, "");
	m_list->SetItem(row, COL_TILEMODE, "");
	m_list->SetItem(row, COL_SLICES, wxString::Format("%d-%d", view.firstSlice, view.firstSlice + view.numSlice - 1));
	m_list->SetItem(row, COL_MIPS, wxString::Format("%d-%d", view.firstMip, view.firstMip + view.numMip - 1));
	m_list->SetItem(row, COL_LAST_ACCESS, "");
}

void TextureRelationViewerWindow::RefreshTextureList()
{
	const int scrollPos = m_list->GetScrollPos(wxVERTICAL);
	m_list->DeleteAllItems();

	std::vector<LatteTextureInformation> textures = LatteTexture_QueryCacheInfo();
	std::stable_sort(textures.begin(), textures.end(), [](const LatteTextureInformation& a, const LatteTextureInformation& b) {
		return a.physAddress < b.physAddress;
	});

	m_list->Freeze();
	long row = 0;
	const uint32 now = GetTickCount();
	for (const LatteTextureInformation& tex : textures)
	{
		if (m_onlyRecent && now - tex.lastAccessTick > kActiveWindowMs)
			continue;
		AddTextureRow(row++, tex);
		if (!m_withViews)
			continue;
		for (const LatteTextureViewInformation& view : tex.views)
			AddViewRow(row++, view);
	}
	m_list->Thaw();

	const long rows = m_list->GetItemCount();
	if (rows > 0)
		m_list->EnsureVisible(std::min<long>(rows - 1, scrollPos + m_list->GetCountPerPage() - 1));
}

void TextureRelationViewerWindow::OnTextureListRightClick(wxMouseEvent& /*event*/)
{
}

void TextureRelationViewerWindow::Close()
{
	Destroy();
}
