// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "helpers/wxHelpers.h"
#include "wxgui/wxgui.h"
#include "wxgui/GraphicPacksWindow2.h"
#include "wxgui/DownloadGraphicPacksWindow.h"
#include "wxgui/DownloadCustomGraphicPackWindow.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"

#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"

#include "wxHelper.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

namespace
{
	// the pack a tree item stands for
	class PackItemData : public wxTreeItemData
	{
	public:
		explicit PackItemData(GraphicPackPtr pack) : m_pack(std::move(pack)) {}
		const GraphicPackPtr& Pack() const { return m_pack; }

	private:
		GraphicPackPtr m_pack;
	};

	// Whether a pack is listed. Universal packs always are. Otherwise, with installedOnly (and a
	// non-empty list of installed titles) one of its titles must be installed, and a filter text
	// must appear (ignoring case) in its tree path or in one of its title ids written in hex.
	bool IsPackListed(bool universal, const std::vector<uint64_t>& packTitles, const std::string& treePath,
					  bool installedOnly, const std::vector<uint64_t>& installed, const std::string& filter)
	{
		if (universal)
			return true;
		auto isInstalled = [&](uint64_t id) { return std::find(installed.begin(), installed.end(), id) != installed.end(); };
		if (installedOnly && !installed.empty() && std::none_of(packTitles.begin(), packTitles.end(), isInstalled))
			return false;
		if (filter.empty() || boost::icontains(treePath, filter))
			return true;
		return std::any_of(packTitles.begin(), packTitles.end(), [&](uint64_t id) { return boost::icontains(fmt::format("{:x}", id), filter); });
	}

	enum class PackSupport
	{
		Supported,
		MaybeNotVulkan, // version 3
		Unsupported,
	};

	PackSupport SupportOfVersion(sint32 version)
	{
		if (version == 3)
			return PackSupport::MaybeNotVulkan;
		if (version >= 4 && version <= 6)
			return PackSupport::Supported;
		if (version == GraphicPack2::GFXPACK_VERSION_7 || version == GraphicPack2::GFXPACK_VERSION_8)
			return PackSupport::Supported;
		return PackSupport::Unsupported;
	}

	const wxString kRestartNotice = wxTRANSLATE("Restart of PixelU required for changes to take effect");
}

GraphicPacksWindow2::GraphicPacksWindow2(wxWindow* parent, uint64_t title_id_filter)
	: wxDialog(parent, wxID_ANY, _("Graphic packs"), wxDefaultPosition, wxSize(1000, 670), wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER),
	  m_installed_games(CafeTitleList::GetAllTitleIds())
{
	if (title_id_filter != 0)
		m_filter = fmt::format("{:x}", title_id_filter);
	m_filter_installed_games = !m_installed_games.empty();

	SetIcon(wxICON(X_BOX));
	SetMinSize(wxSize(500, 400));

	m_splitter_window = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_3D);
	m_splitter_window->Bind(wxEVT_SIZE, &GraphicPacksWindow2::OnSizeChanged, this);
	m_splitter_window->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, &GraphicPacksWindow2::SashPositionChanged, this);

	// left: filter row and tree
	auto* left = new wxPanel(m_splitter_window);
	{
		auto* filterRow = new wxFlexGridSizer(0, 3, 0, 0);
		filterRow->AddGrowableCol(1);
		filterRow->SetFlexibleDirection(wxBOTH);
		filterRow->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

		auto* label = new wxStaticText(left, wxID_ANY, _("Filter"));
		label->Wrap(-1);
		filterRow->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		m_filter_text = new wxTextCtrl(left, wxID_ANY, wxString::FromUTF8(m_filter));
		m_filter_text->Bind(wxEVT_TEXT, &GraphicPacksWindow2::OnFilterUpdate, this);
		filterRow->Add(m_filter_text, 0, wxALL | wxEXPAND, 5);

		m_installed_games_only = new wxCheckBox(left, wxID_ANY, _("Installed games"));
		m_installed_games_only->SetValue(m_filter_installed_games);
		m_installed_games_only->Bind(wxEVT_CHECKBOX, &GraphicPacksWindow2::OnInstalledGamesChanged, this);
		if (m_installed_games.empty())
			m_installed_games_only->Disable();
		filterRow->Add(m_installed_games_only, 0, wxALL | wxEXPAND, 5);

		m_graphic_pack_tree = new wxCheckTree(left, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT);
		m_graphic_pack_tree->Bind(wxEVT_TREE_SEL_CHANGED, &GraphicPacksWindow2::OnTreeSelectionChanged, this);
		m_graphic_pack_tree->Bind(wxEVT_CHECKTREE_CHOICE, &GraphicPacksWindow2::OnTreeChoiceChanged, this);

		auto* column = new wxBoxSizer(wxVERTICAL);
		column->Add(filterRow, 0, wxEXPAND, 5);
		column->Add(m_graphic_pack_tree, 1, wxEXPAND | wxALL, 5);
		left->SetSizerAndFit(column);
	}

	// right: details of the selected pack, then the download buttons
	m_right_panel = new wxPanel(m_splitter_window, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxNO_BORDER | wxFULL_REPAINT_ON_RESIZE);
	{
		m_gp_options = new wxScrolled<wxPanel>(m_right_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxNO_BORDER | wxVSCROLL);
		m_gp_options->SetScrollRate(0, 10);
		m_gp_options->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);

		long textStyle = wxST_NO_AUTORESIZE;
#if wxCHECK_VERSION(3, 3, 2)
		textStyle |= wxST_WRAP;
#endif
		auto addTextBox = [&](wxSizer* into, const wxString& title) {
			auto* group = new wxStaticBoxSizer(new wxStaticBox(m_gp_options, wxID_ANY, title), wxVERTICAL);
			auto* text = new wxStaticText(group->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, textStyle);
			text->SetMinSize(wxSize(0, -1));
			group->Add(text, 1, wxEXPAND | wxALL, 5);
			into->Add(group, 0, wxEXPAND | wxALL, 5);
			return text;
		};

		auto* details = new wxBoxSizer(wxVERTICAL);
		m_graphic_pack_name = addTextBox(details, _("Graphic pack"));
		m_graphic_pack_description = addTextBox(details, _("Description"));

		m_preset_sizer = new wxBoxSizer(wxVERTICAL);
		details->Add(m_preset_sizer, 0, wxEXPAND, 0);

		{
			auto* group = new wxStaticBoxSizer(new wxStaticBox(m_gp_options, wxID_ANY, _("Control")), wxHORIZONTAL);
			m_reload_shaders = new wxButton(group->GetStaticBox(), wxID_ANY, _("Reload edited shaders"));
			m_reload_shaders->Bind(wxEVT_BUTTON, &GraphicPacksWindow2::OnReloadShaders, this);
			m_reload_shaders->Disable();
			group->Add(m_reload_shaders, 0, wxEXPAND | wxALL, 5);
			details->Add(group, 0, wxEXPAND | wxALL, 5);
		}
		details->Add(new wxStaticText(m_gp_options, wxID_ANY, wxEmptyString), 1, wxALL | wxEXPAND, 5);

		m_gp_options->SetSizerAndFit(details);
		m_gp_options->Hide(); // until a pack is selected

		auto* column = new wxBoxSizer(wxVERTICAL);
		column->Add(m_gp_options, 1, wxEXPAND | wxRESERVE_SPACE_EVEN_IF_HIDDEN);
		column->Add(new wxStaticLine(m_right_panel, wxID_ANY), 0, wxLEFT | wxRIGHT | wxEXPAND, 3);

		auto* downloads = new wxWrapSizer(wxHORIZONTAL, 0);
		m_update_graphicPacks = new wxButton(m_right_panel, wxID_ANY, _("Download latest community graphic packs"));
		m_update_graphicPacks->Bind(wxEVT_BUTTON, &GraphicPacksWindow2::OnCheckForUpdates, this);
		downloads->Add(m_update_graphicPacks, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

		m_download_from_url = new wxHyperlinkCtrl(m_right_panel, wxID_ANY, _("Or download pack from URL..."), wxEmptyString);
		m_download_from_url->Bind(wxEVT_HYPERLINK, &GraphicPacksWindow2::OnClickCustomDownload, this);
		downloads->Add(m_download_from_url, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		column->Add(downloads, 0, wxALL | wxEXPAND, 5);

		m_right_panel->SetSizerAndFit(column);
	}

	m_splitter_window->SetMinimumPaneSize(50);
	m_splitter_window->SplitVertically(left, m_right_panel, (sint32)(m_ratio * m_splitter_window->GetParent()->GetSize().GetWidth()));

	m_info_bar = new wxInfoBar(this);
	m_info_bar->SetShowHideEffects(wxSHOW_EFFECT_BLEND, wxSHOW_EFFECT_BLEND);
	m_info_bar->SetEffectDuration(500);

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(m_splitter_window, 1, wxEXPAND, 5);
	layout->Add(m_info_bar, 0, wxALL | wxEXPAND, 5);
	SetSizer(layout);

	UpdateTitleRunning(CafeSystem::IsTitleRunning());
	FillGraphicPackList();
	UpdateAppearance();
	Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& event) {
		event.Skip();
		CallAfter(&GraphicPacksWindow2::UpdateAppearance);
	});
}

GraphicPacksWindow2::~GraphicPacksWindow2()
{
	m_graphic_pack_tree->Unbind(wxEVT_CHECKTREE_CHOICE, &GraphicPacksWindow2::OnTreeChoiceChanged, this);
	m_reload_shaders->Unbind(wxEVT_BUTTON, &GraphicPacksWindow2::OnReloadShaders, this);
	SaveStateToConfig();
}

void GraphicPacksWindow2::RefreshGraphicPacks()
{
	GraphicPack2::ClearGraphicPacks();
	GraphicPack2::LoadAll();
}

// ---- the tree ------------------------------------------------------------------------------------

wxTreeItemId GraphicPacksWindow2::FindTreeItem(const wxTreeItemId& root, const wxString& text) const
{
	wxTreeItemIdValue cookie;
	for (wxTreeItemId item = m_graphic_pack_tree->GetFirstChild(root, cookie); item.IsOk(); item = m_graphic_pack_tree->GetNextSibling(item))
	{
		if (m_graphic_pack_tree->GetItemText(item) == text)
			return item;
	}
	return {};
}

void GraphicPacksWindow2::GetChildren(const wxTreeItemId& id, std::vector<wxTreeItemId>& children) const
{
	wxTreeItemIdValue cookie;
	for (wxTreeItemId child = m_graphic_pack_tree->GetFirstChild(id, cookie); child.IsOk(); child = m_graphic_pack_tree->GetNextChild(child, cookie))
		children.emplace_back(child);
}

void GraphicPacksWindow2::ExpandChildren(const std::vector<wxTreeItemId>& ids, size_t& counter) const
{
	// opens the tree level by level while fewer than 30 items are visible
	std::vector<wxTreeItemId> nextLevel;
	for (const auto& id : ids)
		GetChildren(id, nextLevel);
	counter += nextLevel.size();
	if (counter >= 30 || nextLevel.empty())
		return;

	const wxTreeItemId root = m_graphic_pack_tree->GetRootItem();
	for (const auto& id : ids)
	{
		if (id != root && m_graphic_pack_tree->HasChildren(id))
			m_graphic_pack_tree->Expand(id);
	}
	ExpandChildren(nextLevel, counter);
}

void GraphicPacksWindow2::FillGraphicPackList() const
{
	wxWindowUpdateLocker freeze(m_graphic_pack_tree);

	m_graphic_pack_tree->DeleteAllItems();
	const wxTreeItemId root = m_graphic_pack_tree->AddRoot("Root");

	for (const auto& pack : GraphicPack2::GetGraphicPacks())
	{
		if (!IsPackListed(pack->IsUniversal(), pack->GetTitleIds(), pack->GetVirtualPath(), m_filter_installed_games, m_installed_games, m_filter))
			continue;

		// folders along the tree path, then the pack's own item ("name #2", "#3"... when taken)
		const auto parts = TokenizeView(pack->GetVirtualPath(), '/');
		wxTreeItemId node = root;
		for (size_t i = 0; i < parts.size(); ++i)
		{
			const wxTreeItemId parent = node;
			const wxString part = wxString::FromUTF8(parts[i]);
			if (i + 1 < parts.size())
			{
				node = FindTreeItem(parent, part);
				if (!node.IsOk())
					node = m_graphic_pack_tree->AppendItem(parent, part);
				continue;
			}
			for (sint32 copy = 1; copy <= 999; ++copy)
			{
				const wxString name = copy == 1 ? part : part + formatWxString(" #{}", copy);
				node = FindTreeItem(parent, name);
				if (!node.IsOk())
				{
					node = m_graphic_pack_tree->AppendItem(parent, name);
					break;
				}
			}
		}
		if (!node.IsOk() || node == root)
			continue;

		m_graphic_pack_tree->SetItemData(node, new PackItemData(pack));
		const PackSupport support = SupportOfVersion(pack->GetVersion());
		if (support == PackSupport::MaybeNotVulkan)
			m_graphic_pack_tree->SetItemText(node, m_graphic_pack_tree->GetItemText(node) + " (may not be compatible with Vulkan)");
		else if (support == PackSupport::Unsupported)
		{
			m_graphic_pack_tree->SetItemText(node, m_graphic_pack_tree->GetItemText(node) + " (Unsupported version)");
			m_graphic_pack_tree->SetItemTextColour(node, m_incompatible_colour);
		}
		else if (pack->IsActivated())
			m_graphic_pack_tree->SetItemTextColour(node, m_activated_colour);

		m_graphic_pack_tree->MakeCheckable(node, pack->IsEnabled());
		if (support == PackSupport::Unsupported)
			m_graphic_pack_tree->DisableCheckBox(node);
	}

	m_graphic_pack_tree->Sort(root, true);

	if (!m_filter.empty())
	{
		size_t shown = 0;
		ExpandChildren({root}, shown);
	}
}

void GraphicPacksWindow2::UpdateAppearance()
{
	// re-pick the colours for the current (light/dark) theme and recolour the items
	const wxColour oldActivated = m_activated_colour;
	const wxColour oldIncompatible = m_incompatible_colour;
	m_default_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	m_activated_colour = wxSystemSettings::SelectLightDark(wxColour(0x00, 0x80, 0x00), wxColour(0x42, 0xB3, 0x42));
	m_incompatible_colour = wxSystemSettings::SelectLightDark(wxColour(0xCC, 0x00, 0x00), wxColour(0xDE, 0x49, 0x49));

	std::vector<wxTreeItemId> items;
	const wxTreeItemId root = m_graphic_pack_tree->GetRootItem();
	if (root.IsOk())
		GetChildren(root, items);
	for (size_t i = 0; i < items.size(); ++i) // grows while walking: every level gets visited
	{
		const wxTreeItemId item = items[i];
		const wxColour current = m_graphic_pack_tree->GetItemTextColour(item);
		const wxColour& next = current == oldActivated ? m_activated_colour : current == oldIncompatible ? m_incompatible_colour : m_default_colour;
		m_graphic_pack_tree->SetItemTextColour(item, next);
		GetChildren(item, items);
	}

	const wxColour link = wxSystemSettings::SelectLightDark(wxColour(0x00, 0x55, 0xAA), wxColour(0x80, 0xBD, 0xFF));
	m_download_from_url->SetNormalColour(link);
	m_download_from_url->SetHoverColour(link);
	m_download_from_url->SetVisitedColour(wxSystemSettings::SelectLightDark(wxColour(0x70, 0x36, 0x9B), wxColour(0xCC, 0xA9, 0xFF)));
	Refresh();
}

void GraphicPacksWindow2::OnFilterUpdate(wxEvent& event)
{
	m_filter = m_filter_text->GetValue().utf8_string();
	FillGraphicPackList();
	event.Skip();
}

void GraphicPacksWindow2::OnInstalledGamesChanged(wxCommandEvent& event)
{
	m_filter_installed_games = m_installed_games_only->GetValue();
	FillGraphicPackList();
	event.Skip();
}

// ---- selected pack -------------------------------------------------------------------------------

void GraphicPacksWindow2::UpdateOptionsLayout()
{
	m_right_panel->Layout();
	m_gp_options->Layout();
	m_gp_options->FitInside();
}

void GraphicPacksWindow2::LoadPresetSelections(const GraphicPackPtr& gp)
{
	// one choice per preset category that has a visible preset
	std::vector<std::string> categoryOrder;
	auto byCategory = gp->GetCategorizedPresets(categoryOrder);
	for (const std::string& category : categoryOrder)
	{
		const auto& presets = byCategory[category];
		if (std::none_of(presets.cbegin(), presets.cend(), [](const auto& p) { return p->visible; }))
			continue;

		const wxString categoryName = wxString::FromUTF8(category);
		auto* group = new wxStaticBoxSizer(new wxStaticBox(m_preset_sizer->GetContainingWindow(), wxID_ANY, category.empty() ? _("Active preset") : categoryName), wxVERTICAL);

		auto* choice = new wxChoice(group->GetStaticBox(), wxID_ANY);
		choice->SetMinSize(wxSize(0, -1));
		choice->SetClientObject(new wxStringClientData(categoryName));
		choice->Bind(wxEVT_CHOICE, &GraphicPacksWindow2::OnActivePresetChanged, this);

		std::optional<std::string> active;
		for (const auto& preset : presets)
		{
			if (!preset->visible)
				continue;
			choice->AppendString(wxString::FromUTF8(preset->name));
			if (preset->active)
				active = preset->name;
		}
		if (active)
			choice->SetStringSelection(wxString::FromUTF8(*active));
		else if (choice->GetCount() > 0)
			choice->SetSelection(0);

		group->Add(choice, 1, wxEXPAND | wxALL, 5);
		m_preset_sizer->Add(group, 0, wxEXPAND | wxALL, 5);
	}
}

// Removes the preset boxes without destroying them inside the handler of one of their choices
// (that crashes some GTK set-ups, e.g. flatpak): they are hidden now and destroyed afterwards.
void GraphicPacksWindow2::ClearPresets()
{
	std::vector<wxStaticBoxSizer*> groups;
	for (size_t i = 0; i < m_preset_sizer->GetItemCount(); ++i)
	{
		if (auto* group = dynamic_cast<wxStaticBoxSizer*>(m_preset_sizer->GetItem(i)->GetSizer()))
			groups.push_back(group);
	}
	for (wxStaticBoxSizer* group : groups)
	{
		wxStaticBox* box = group->GetStaticBox();
		if (!box)
			continue;
		m_preset_sizer->Detach(group);
		box->Hide();
		CallAfter([box, group]() {
			box->DestroyChildren();
			delete group; // takes the box with it
		});
	}
}

void GraphicPacksWindow2::OnTreeSelectionChanged(wxTreeEvent&)
{
	wxWindowUpdateLocker freeze(this);

	const wxTreeItemId selection = m_graphic_pack_tree->GetSelection();
	if (selection == m_graphic_pack_tree->GetRootItem())
		return;

	const auto* data = selection.IsOk() ? dynamic_cast<PackItemData*>(m_graphic_pack_tree->GetItemData(selection)) : nullptr;
	if (!data)
	{
		// a folder: nothing to show
		m_preset_sizer->Clear(true);
		m_graphic_pack_name->SetLabel(wxEmptyString);
		m_graphic_pack_description->SetLabel(wxEmptyString);
		m_reload_shaders->Disable();
		m_shown_graphic_pack.reset();
		m_gp_options->Hide();
		UpdateOptionsLayout();
		return;
	}

	if (!m_gp_options->IsShown())
		m_gp_options->Show();
	const GraphicPackPtr& pack = data->Pack();
	if (pack == m_shown_graphic_pack)
		return;

	m_preset_sizer->Clear(true);
	m_graphic_pack_name->SetLabel(wxString::FromUTF8(pack->GetName()));
	m_graphic_pack_description->SetLabel(pack->GetDescription().empty() ? _("This graphic pack has no description") : wxString::FromUTF8(pack->GetDescription()));
	LoadPresetSelections(pack);
	m_reload_shaders->Enable(pack->HasShaders());
	m_shown_graphic_pack = pack;
	UpdateOptionsLayout();
}

void GraphicPacksWindow2::OnTreeChoiceChanged(wxTreeEvent& event)
{
	const wxTreeItemId item = event.GetItem();
	if (!item.IsOk())
		return;
	const auto* data = dynamic_cast<PackItemData*>(m_graphic_pack_tree->GetItemData(item));
	if (!data)
		return;

	const bool enable = event.GetExtraLong() != 0;
	const GraphicPackPtr& pack = data->Pack();
	pack->SetEnabled(enable);

	// a pack for the running game is switched on or off right away where possible
	const bool needsRestart = pack->RequiresRestart(true, false);
	const bool affectsRunningGame = CafeSystem::IsTitleRunning() && pack->ContainsTitleId(CafeSystem::GetForegroundTitleId());
	if (affectsRunningGame)
	{
		if (enable)
		{
			GraphicPack2::ActivateGraphicPack(pack);
			if (!needsRestart)
			{
				ReloadPack(pack);
				m_graphic_pack_tree->SetItemTextColour(item, m_activated_colour);
			}
		}
		else
		{
			if (!needsRestart)
			{
				DeleteShadersFromRuntimeCache(pack);
				m_graphic_pack_tree->SetItemTextColour(item, m_default_colour);
			}
			GraphicPack2::DeactivateGraphicPack(pack);
		}
		if (needsRestart && !m_info_bar->IsShown())
			m_info_bar->ShowMessage(wxGetTranslation(kRestartNotice));
	}

	m_graphic_pack_tree->SelectItem(item);
}

void GraphicPacksWindow2::OnActivePresetChanged(wxCommandEvent& event)
{
	if (!m_shown_graphic_pack)
		return;

	auto* choice = wxDynamicCast(event.GetEventObject(), wxChoice);
	wxASSERT(choice);
	auto* category = dynamic_cast<wxStringClientData*>(choice->GetClientObject());
	wxASSERT(category);

	// a different preset can change which other presets are visible
	if (m_shown_graphic_pack->SetActivePreset(category->GetData().utf8_string(), choice->GetStringSelection().utf8_string()))
	{
		wxWindowUpdateLocker freeze(this);
		ClearPresets();
		LoadPresetSelections(m_shown_graphic_pack);
		UpdateOptionsLayout();
	}

	if (!m_shown_graphic_pack->RequiresRestart(false, true))
		ReloadPack(m_shown_graphic_pack);
	else if (!m_info_bar->IsShown())
		m_info_bar->ShowMessage(wxGetTranslation(kRestartNotice));
}

void GraphicPacksWindow2::OnReloadShaders(wxCommandEvent&)
{
	if (m_shown_graphic_pack)
		ReloadPack(m_shown_graphic_pack);
}

void GraphicPacksWindow2::ReloadPack(const GraphicPackPtr& graphic_pack) const
{
	const bool hasRuntimeParts = graphic_pack->HasShaders() || graphic_pack->HasPatches() || graphic_pack->HasCustomVSyncFrequency();
	if (hasRuntimeParts && graphic_pack->Reload())
		DeleteShadersFromRuntimeCache(graphic_pack);
}

void GraphicPacksWindow2::DeleteShadersFromRuntimeCache(const GraphicPackPtr& graphic_pack) const
{
	for (const auto& shader : graphic_pack->GetCustomShaders())
	{
		LatteConst::ShaderType type;
		switch (shader.type)
		{
		case GraphicPack2::GP_SHADER_TYPE::VERTEX: type = LatteConst::ShaderType::Vertex; break;
		case GraphicPack2::GP_SHADER_TYPE::GEOMETRY: type = LatteConst::ShaderType::Geometry; break;
		case GraphicPack2::GP_SHADER_TYPE::PIXEL: type = LatteConst::ShaderType::Pixel; break;
		default: continue;
		}
		LatteAsyncCommands_queueDeleteShader(shader.shader_base_hash, shader.shader_aux_hash, type);
	}
}

// ---- downloads -----------------------------------------------------------------------------------

void GraphicPacksWindow2::OnClickCustomDownload(wxCommandEvent&)
{
	DownloadCustomGraphicPackWindow download(this);
	if (download.ShowModal() == wxID_OK)
	{
		RefreshGraphicPacks();
		FillGraphicPackList();
	}
}

void GraphicPacksWindow2::OnCheckForUpdates(wxCommandEvent&)
{
	DownloadGraphicPacksWindow download(this);
	SaveStateToConfig();
	if (download.ShowModal() != wxID_OK || CafeSystem::IsTitleRunning())
		return;

	// tell the user about enabled packs the new release no longer has (removed or renamed)
	std::map<std::string, std::string> enabledBefore; // file path -> tree path
	for (const auto& pack : GraphicPack2::GetGraphicPacks())
	{
		if (pack->IsEnabled())
			enabledBefore.emplace(pack->GetNormalizedPathString(), pack->GetVirtualPath());
	}
	RefreshGraphicPacks();
	FillGraphicPackList();
	for (const auto& pack : GraphicPack2::GetGraphicPacks())
		enabledBefore.erase(pack->GetNormalizedPathString());
	if (enabledBefore.empty())
		return;

	std::string names;
	for (const auto& entry : enabledBefore)
		names.append(entry.second).push_back('\n');
	wxString message = _("This update removed or renamed the following graphic packs:");
	message << "\n \n" << wxString::FromUTF8(names) << " \n" << _("You may need to set them up again.");
	wxMessageBox(message, _("Warning"), wxOK | wxCENTRE | wxICON_INFORMATION, this);
}

void GraphicPacksWindow2::UpdateTitleRunning(bool running)
{
	m_update_graphicPacks->Enable(!running);
	m_download_from_url->Enable(!running);
	if (running)
	{
		const wxString why = _("Graphic packs cannot be updated while a game is running.");
		m_download_from_url->SetToolTip(why);
		m_update_graphicPacks->SetToolTip(why);
	}
	else
	{
		m_update_graphicPacks->SetToolTip(nullptr);
		m_download_from_url->SetToolTip(nullptr);
	}
}

// ---- splitter ------------------------------------------------------------------------------------

void GraphicPacksWindow2::OnSizeChanged(wxSizeEvent& event)
{
	// keep the left pane at the same share of the width
	auto* splitter = (wxSplitterWindow*)event.GetEventObject();
	wxASSERT(splitter);
	const int width = std::max(splitter->GetMinimumPaneSize(), splitter->GetParent()->GetClientSize().GetWidth());
	splitter->SetSashPosition((sint32)(m_ratio * width));
	event.Skip();
}

void GraphicPacksWindow2::SashPositionChanged(wxEvent& event)
{
	auto* splitter = (wxSplitterWindow*)event.GetEventObject();
	wxASSERT(splitter);
	const int width = std::max(splitter->GetMinimumPaneSize(), splitter->GetParent()->GetClientSize().GetWidth());
	m_ratio = (float)splitter->GetSashPosition() / width;
	event.Skip();
}

// ---- settings ------------------------------------------------------------------------------------

void GraphicPacksWindow2::SaveStateToConfig()
{
	// enabled packs with their chosen presets, and default-on packs the user switched off
	auto& entries = GetConfigHandle().data().graphic_pack_entries;
	entries.clear();
	for (const auto& pack : GraphicPack2::GetGraphicPacks())
	{
		const fs::path file = _utf8ToPath(pack->GetNormalizedPathString());
		if (pack->IsEnabled())
		{
			auto& presets = entries[file];
			for (const auto& preset : pack->GetActivePresets())
				presets.try_emplace(preset->category, preset->name);
		}
		else if (pack->IsDefaultEnabled())
			entries[file].try_emplace("_disabled", "false");
	}
	GetConfigHandle().Save();
}
