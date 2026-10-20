// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/debugger/DebuggerWindow2.h"

#include "wxHelper.h"

#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/RPL/rpl_debug_symbols.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/OS/RPL/rpl.h"
#include "wxgui/debugger/RegisterWindow.h"
#include "wxgui/debugger/DumpWindow.h"
#include "wxgui/debugger/DisasmCtrl.h"
#include "wxgui/debugger/SymbolWindow.h"
#include "wxgui/debugger/BreakpointWindow.h"
#include "wxgui/debugger/ModuleWindow.h"
#include "util/helpers/helpers.h"
#include "util/helpers/StringHelpers.h"
#include "Cemu/Logging/CemuLogging.h"

#include "resource/embedded/resources.h"

#include <charconv>

wxDEFINE_EVENT(wxEVT_DEBUGGER_CLOSE, wxCloseEvent);
wxDEFINE_EVENT(wxEVT_UPDATE_VIEW, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_BREAKPOINT_CHANGE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_BREAKPOINT_HIT, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_MOVE_TO_DISASM_ADDR, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_RUN, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_NOTIFY_MODULE_LOADED, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_NOTIFY_MODULE_UNLOADED, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_NOTIFY_GRAPHIC_PACKS_MODIFIED, wxCommandEvent);

DebuggerWindow2* s_debuggerWindow;

namespace
{
	enum
	{
		ID_EXIT = wxID_HIGHEST + 8000,

		ID_OPT_PIN,
		ID_OPT_BREAK_ON_ENTRY,
		ID_OPT_LOG_MEMORY_BPS,
		ID_OPT_INTERPRETER,

		ID_WIN_REGISTERS,
		ID_WIN_DUMP,
		ID_WIN_BREAKPOINTS,
		ID_WIN_MODULES,
		ID_WIN_SYMBOLS,

		ID_TOOL_GOTO,
		ID_TOOL_BREAKPOINT,
		ID_TOOL_RUN_BREAK,
		ID_TOOL_STEP_OVER,
		ID_TOOL_STEP_INTO,
	};

	// carries a copy of the module description across the thread hop
	struct ModuleNotice : public wxClientData
	{
		explicit ModuleNotice(RPLModule* module) : info(module) {}
		DebuggerModuleInfo info;
	};

	// ---- .map file parsing ----

	constexpr const char* kMapPunctuation = "\t\n\v\f\r ,:;()[]";

	// accepts "80001234", "0x80001234", "80001234h" and "0001:80001234"
	bool ParseMapAddress(std::string_view text, uint32& out)
	{
		trim(text, kMapPunctuation);
		const size_t colon = text.rfind(':');
		if (colon != std::string_view::npos)
			text.remove_prefix(colon + 1);
		trim(text, kMapPunctuation);
		if (!text.empty() && (text.back() == 'h' || text.back() == 'H'))
			text.remove_suffix(1);
		if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
			text.remove_prefix(2);
		if (text.empty())
			return false;
		uint64 v = 0;
		const char* last = text.data() + text.size();
		const auto res = std::from_chars(text.data(), last, v, 16);
		if (res.ec != std::errc() || res.ptr != last || v > 0xFFFFFFFFull)
			return false;
		out = (uint32)v;
		return true;
	}

	// "<address> <symbol> [anything]" with blanks or tabs between the fields
	bool ParseMapLine(std::string_view line, uint32& address, std::string& name)
	{
		constexpr const char* kBlank = " \t";
		trim(line);
		const size_t split = line.find_first_of(kBlank);
		if (line.empty() || split == std::string_view::npos)
			return false;
		if (!ParseMapAddress(line.substr(0, split), address))
			return false;
		std::string_view rest = line.substr(split + 1);
		trim(rest);
		std::string_view symbol = rest.substr(0, rest.find_first_of(kBlank));
		trim(symbol);
		if (symbol.empty())
			return false;
		name.assign(symbol.data(), symbol.size());
		return true;
	}

	// map files use the addresses of the unrelocated RPL
	std::optional<MPTR> RelocateMapAddress(const DebuggerModuleInfo& info, uint32 addr)
	{
		for (const DebuggerModuleInfo::ModuleArea* area : {&info.textArea, &info.dataArea})
		{
			if (area->ContainsOriginalAddress(addr))
				return area->base + (addr - area->origBase);
		}
		return std::nullopt;
	}

	bool IsCodeBreakpointType(sint32 type)
	{
		return type == DEBUGGER_BP_T_NORMAL || type == DEBUGGER_BP_T_LOGGING;
	}

	std::optional<uint32> ModuleRelative(const DebuggerModuleInfo::ModuleArea& area, uint32 addr)
	{
		if (!area.ContainsAddress(addr))
			return std::nullopt;
		return addr - area.base;
	}

	void SetInterpreterOnly(bool interpreter)
	{
		if (interpreter)
		{
			PPCRecompiler_Disable();
			cemuLog_log(LogType::Force, "Debugger: Switched CPU mode to interpreter");
		}
		else
		{
			PPCRecompiler_Enable();
			cemuLog_log(LogType::Force, "Debugger: Switched CPU mode to recompiler");
		}
	}

	wxColour ToolTint()
	{
		return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	}

	template<size_t N>
	wxBitmap Icon(const unsigned char (&png)[N], const wxColour& tint)
	{
		return wxHelper::LoadThemedBitmapFromPNG(png, N, tint);
	}

	wxString ModuleCaption(const char* name)
	{
		return wxString::Format("> %s", name);
	}
}

// ---------------------------------------------------------------------------
// configuration

DebuggerModuleInfo::DebuggerModuleInfo(RPLModule* module)
	: moduleName(module->moduleName), patchCRC(module->patchCRC)
{
	textArea = {module->regionMappingBase_text.GetMPTR(), module->regionOrigAddr_text, module->regionSize_text};
	dataArea = {module->regionMappingBase_data, module->regionOrigAddr_data, module->regionSize_data};
}

void DebuggerConfig::Load(XMLConfigParser& parser)
{
	pin_to_main = parser.get("PinToMainWindow", true);
	break_on_start = parser.get("break_on_start", true);
	log_memory_breakpoints = parser.get("log_memory_breakpoints", false);

	auto windows = parser.get("Windows");
	show_register = windows.get("Registers", true);
	show_dump = windows.get("MemoryDump", true);
	show_stack = windows.get("Stack", true);
	show_breakpoints = windows.get("Breakpoints", true);
	show_modules = windows.get("Modules", true);
	show_symbols = windows.get("Symbols", true);
}

void DebuggerConfig::Save(XMLConfigParser& parser)
{
	parser.set("PinToMainWindow", pin_to_main);
	parser.set("break_on_start", break_on_start);
	parser.set("log_memory_breakpoints", log_memory_breakpoints);

	auto windows = parser.set("Windows");
	windows.set("Registers", show_register);
	windows.set("MemoryDump", show_dump);
	windows.set("Stack", show_stack);
	windows.set("Breakpoints", show_breakpoints);
	windows.set("Modules", show_modules);
	windows.set("Symbols", show_symbols);
}

void DebuggerModuleStorage::Load(XMLConfigParser& parser)
{
	auto bpList = parser.get("Breakpoints");
	for (auto entry = bpList.get("Entry"); entry.valid(); entry = bpList.get("Entry", entry))
	{
		const uint32 offset = std::strtoul(entry.get("Address", ""), nullptr, 16);
		if (offset == 0)
			continue;
		const sint32 type = entry.get("Type", 0);
		const bool enabled = entry.get("Enabled", true);
		const std::string comment = entry.get("Comment", "");

		const uint32 address = (IsCodeBreakpointType(type) ? moduleInfo.textArea.base : moduleInfo.dataArea.base) + offset;
		// a breakpoint already set here wins over the stored one
		if (debugger_getFirstBP(address) != nullptr)
			continue;

		switch (type)
		{
		case DEBUGGER_BP_T_NORMAL:
		case DEBUGGER_BP_T_LOGGING:
			debugger_createCodeBreakpoint(address, (uint8)type);
			break;
		case DEBUGGER_BP_T_MEMORY_READ:
			debugger_createMemoryBreakpoint(address, true, false);
			break;
		case DEBUGGER_BP_T_MEMORY_WRITE:
			debugger_createMemoryBreakpoint(address, false, true);
			break;
		default:
			continue;
		}

		DebuggerBreakpoint* created = debugger_getFirstBP(address);
		if (!enabled)
			debugger_toggleBreakpoint(address, false, created);
		created->comment = boost::nowide::widen(comment);
	}

	auto commentList = parser.get("Comments");
	for (auto entry = commentList.get("Entry"); entry.valid(); entry = commentList.get("Entry", entry))
	{
		const uint32 address = std::strtoul(entry.get("Address", ""), nullptr, 16);
		const char* text = entry.get("Comment", "");
		if (address == 0 || *text == '\0')
			continue;
		rplDebugSymbol_createComment(address, boost::nowide::widen(text).c_str());
	}
}

void DebuggerModuleStorage::Save(XMLConfigParser& parser)
{
	// breakpoints: the first one at each address, stored relative to its section
	std::vector<BreakpointId> toDelete;
	auto bpList = parser.set("Breakpoints");
	for (DebuggerBreakpoint* bp : debugger_lockBreakpoints())
	{
		if (bp->dbType != DEBUGGER_BP_T_DEBUGGER)
			continue;
		std::optional<uint32> offset = ModuleRelative(moduleInfo.textArea, bp->address);
		if (!offset)
			offset = ModuleRelative(moduleInfo.dataArea, bp->address);
		if (!offset)
			continue;

		auto entry = bpList.set("Entry");
		entry.set("Address", fmt::format("{:#10x}", *offset));
		entry.set("Comment", boost::nowide::narrow(bp->comment).c_str());
		entry.set("Type", bp->bpType);
		entry.set("Enabled", bp->enabled);
		if (delete_breakpoints_after_saving)
			toDelete.emplace_back(bp->id);
	}
	debugger_unlockBreakpoints();
	// deleting reshapes the breakpoint list, so do it after walking it
	for (BreakpointId id : toDelete)
		debugger_deleteBreakpoint(id);
	delete_breakpoints_after_saving = false;

	// comments inside this module's code
	auto commentList = parser.set("Comments");
	for (const auto& [address, symbol] : rplDebugSymbol_getSymbols())
	{
		const auto* comment = static_cast<rplDebugSymbolComment*>(symbol);
		if (comment == nullptr || comment->type != RplDebugSymbolComment)
			continue;
		const std::optional<uint32> offset = ModuleRelative(moduleInfo.textArea, address);
		if (!offset)
			continue;
		auto entry = commentList.set("Entry");
		entry.set("Address", fmt::format("{:#10x}", *offset));
		entry.set("Comment", boost::nowide::narrow(comment->comment).c_str());
	}
}

// ---------------------------------------------------------------------------
// module storage

std::wstring DebuggerWindow2::GetModuleStoragePath(std::string module_name, uint32_t crc_hash) const
{
	if (module_name.empty() || crc_hash == 0)
		return {};
	return ActiveSettings::GetConfigPath("debugger/{}_{:#10x}.xml", module_name, crc_hash).generic_wstring();
}

std::wstring DebuggerWindow2::GetModuleMapPath(std::string module_name, uint32_t crc_hash) const
{
	if (module_name.empty() || crc_hash == 0)
		return {};
	return ActiveSettings::GetConfigPath("debugger/{}_{:#10x}.map", module_name, crc_hash).generic_wstring();
}

void DebuggerWindow2::LoadModuleStorage(const DebuggerModuleInfo& moduleInfo)
{
	const std::wstring path = GetModuleStoragePath(moduleInfo.moduleName, moduleInfo.patchCRC);
	if (path.empty())
		return;
	for (const auto& existing : m_modulesStorage)
	{
		if (existing->GetFilename() == path)
			return;
	}
	m_modulesStorage.emplace_back(std::make_unique<XMLDebuggerModuleConfig>(path, DebuggerModuleStorage{moduleInfo, false}));
	XMLDebuggerModuleConfig& storage = *m_modulesStorage.back();
	storage.Load();
	LoadModuleMap(storage.data());
}

void DebuggerWindow2::SaveModuleStorage(const DebuggerModuleInfo& moduleInfo, bool deleteModuleStorage)
{
	const std::wstring path = GetModuleStoragePath(moduleInfo.moduleName, moduleInfo.patchCRC);
	auto match = std::find_if(m_modulesStorage.begin(), m_modulesStorage.end(), [&](const std::unique_ptr<XMLDebuggerModuleConfig>& s) {
		const DebuggerModuleInfo& info = s->data().moduleInfo;
		return info.moduleName == moduleInfo.moduleName && info.patchCRC == moduleInfo.patchCRC;
	});
	if (match == m_modulesStorage.end())
		return;
	// dropping the storage also drops the module's breakpoints
	(*match)->data().delete_breakpoints_after_saving = deleteModuleStorage;
	(*match)->Save(path);
	if (!deleteModuleStorage)
		return;
	UnloadModuleMap((*match)->data());
	m_modulesStorage.erase(match);
}

void DebuggerWindow2::LoadModuleMap(DebuggerModuleStorage& storage)
{
	UnloadModuleMap(storage);
	const DebuggerModuleInfo& info = storage.moduleInfo;
	const std::wstring path = GetModuleMapPath(info.moduleName, info.patchCRC);
	if (path.empty())
		return;
	std::unique_ptr<FileStream> file(FileStream::openFile(path.c_str()));
	if (!file)
		return;
	const uint64 size = file->GetSize();
	if (size == 0 || size > 0xFFFFFFFFull)
		return;
	std::vector<char> text((size_t)size);
	if (file->readData(text.data(), (uint32)size) != (uint32)size)
		return;

	for (std::string_view line : StringHelpers::StringLineIterator(std::string_view(text.data(), text.size())))
	{
		uint32 mapAddr = 0;
		std::string name;
		if (!ParseMapLine(line, mapAddr, name))
			continue;
		if (const std::optional<MPTR> addr = RelocateMapAddress(info, mapAddr))
			storage.loaded_map_symbols.emplace_back(rplSymbolStorage_store(info.moduleName.c_str(), name.c_str(), *addr, RPL_STORED_SYMBOL_MAP));
	}
}

void DebuggerWindow2::UnloadModuleMap(DebuggerModuleStorage& storage)
{
	for (RPLStoredSymbol* sym : storage.loaded_map_symbols)
	{
		if (sym != nullptr)
			rplSymbolStorage_remove(sym);
	}
	storage.loaded_map_symbols.clear();
}

// ---------------------------------------------------------------------------
// window

DebuggerWindow2::DebuggerWindow2(wxFrame& parent, const wxRect& display_size)
	: wxFrame(&parent, wxID_ANY, _("PPC Debugger"), wxDefaultPosition, wxSize(1280, 300), wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT)
{
	g_debuggerDispatcher.SetDebuggerCallbacks(this);

	m_config.SetFilename(ActiveSettings::GetConfigPath("debugger/config.xml").generic_wstring());
	m_config.Load();
	debugger_setOptionBreakOnEntry(m_config.data().break_on_start);
	debugger_setOptionLogOnlyMemoryBreakpoints(m_config.data().log_memory_breakpoints);

	m_mainPos = parent.GetPosition();
	m_mainSize = parent.GetSize();
	// 80% of the screen height left over by the main window, at least 300px
	const double spare = (display_size.GetHeight() - 800) * 0.8;
	SetSize(1280, spare > 300.0 ? (int)spare : 300);

	BuildMenus();
	CreateToolBar();

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(m_toolbar, 0, wxEXPAND);

	// modules that were loaded before the debugger opened
	const auto modules = RPLLoader_GetModuleList();
	for (RPLModule* mod : modules)
		LoadModuleStorage(DebuggerModuleInfo(mod));

	wxString caption = _("> no modules loaded");
	if (!modules.empty())
	{
		if (RPLModule* main = RPLLoader_FindModuleByCodeAddr(MEMORY_CODEAREA_ADDR))
			caption = ModuleCaption(main->moduleName.c_str());
		else
			caption = _("> unknown module");
	}
	m_moduleLabel = new wxStaticText(this, wxID_ANY, caption);
	layout->Add(m_moduleLabel, 0, wxEXPAND | wxALL, 5);

	m_disasm = new DisasmCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxScrolledWindowStyle);
	layout->Add(m_disasm, 1, wxEXPAND);
	SetSizer(layout);
	wxWindowBase::Layout();

	m_registers = new RegisterWindow(*this, m_mainPos, m_mainSize);
	m_dump = new DumpWindow(*this, m_mainPos, m_mainSize);
	m_breakpoints = new BreakpointWindow(*this, m_mainPos, m_mainSize);
	m_modules = new ModuleWindow(*this, m_mainPos, m_mainSize);
	m_symbols = new SymbolWindow(*this, m_mainPos, m_mainSize);

	// the first layout always docks to the main window, whatever the pin option says
	const bool pinned = m_config.data().pin_to_main;
	m_config.data().pin_to_main = true;
	OnParentMove(m_mainPos, m_mainSize);
	m_config.data().pin_to_main = pinned;

	Bind(wxEVT_SHOW, &DebuggerWindow2::OnShowEvent, this);
	Bind(wxEVT_CLOSE_WINDOW, &DebuggerWindow2::OnCloseEvent, this);
	Bind(wxEVT_MENU, &DebuggerWindow2::OnToolbar, this, ID_TOOL_GOTO, ID_TOOL_STEP_INTO);
	Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, ID_EXIT);
	Bind(wxEVT_MENU, &DebuggerWindow2::OnOptionsMenu, this, ID_OPT_PIN, ID_OPT_INTERPRETER);
	Bind(wxEVT_MENU, &DebuggerWindow2::OnWindowMenu, this, ID_WIN_REGISTERS, ID_WIN_SYMBOLS);

	// views
	Bind(wxEVT_UPDATE_VIEW, [this](wxCommandEvent&) {
		ShowModuleOf();
		m_disasm->OnUpdateView();
		m_registers->OnUpdateView();
		m_breakpoints->OnUpdateView();
	});
	Bind(wxEVT_BREAKPOINT_CHANGE, [this](wxCommandEvent&) {
		m_breakpoints->OnUpdateView();
		m_disasm->RefreshControl();
		ShowModuleOf();
	});
	Bind(wxEVT_MOVE_TO_DISASM_ADDR, [this](wxCommandEvent& evt) {
		const MPTR target = (MPTR)(uintptr_t)evt.GetClientData();
		if (target == 0)
			return;
		ShowModuleOf(target);
		m_disasm->CenterOffset(target);
	});
	Bind(wxEVT_DISASMCTRL_NOTIFY_GOTO_ADDRESS, [this](wxCommandEvent& evt) {
		ShowModuleOf((uint32)evt.GetExtraLong());
	});

	// execution state
	Bind(wxEVT_BREAKPOINT_HIT, [this](wxCommandEvent&) {
		PPCInterpreter_t* cpu = debugger_lockDebugSession();
		if (cpu == nullptr)
			return;
		const MPTR ip = cpu->instructionPointer;
		debugger_unlockDebugSession(cpu);
		ShowModuleOf(ip);
		SetTrappedState(true);
		m_disasm->CenterOffset(ip);
	});
	Bind(wxEVT_RUN, [this](wxCommandEvent&) { SetTrappedState(false); });

	// module list changes
	Bind(wxEVT_NOTIFY_MODULE_LOADED, [this](wxCommandEvent& evt) {
		LoadModuleStorage(static_cast<ModuleNotice*>(evt.GetClientObject())->info);
		ReloadModuleViews();
	});
	Bind(wxEVT_NOTIFY_MODULE_UNLOADED, [this](wxCommandEvent& evt) {
		SaveModuleStorage(static_cast<ModuleNotice*>(evt.GetClientObject())->info, true);
		ReloadModuleViews();
	});
	Bind(wxEVT_NOTIFY_GRAPHIC_PACKS_MODIFIED, [this](wxCommandEvent&) { ReloadModuleViews(); });

	s_debuggerWindow = this;
	ApplyTheme();
	Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& evt) {
		evt.Skip();
		CallAfter(&DebuggerWindow2::ApplyTheme);
	});
}

DebuggerWindow2::~DebuggerWindow2()
{
	g_debuggerDispatcher.ClearDebuggerCallbacks();
	s_debuggerWindow = nullptr;
	CleanupForDestroy();

	ForEachToolWindow([](wxFrame& w) {
		if (w.IsShown())
			w.Close(true);
	});

	if (m_forceInterpreter)
	{
		m_forceInterpreter = false;
		SetInterpreterOnly(false);
	}
}

void DebuggerWindow2::CleanupForDestroy()
{
	debugger_setOptionBreakOnEntry(false);
	while (!m_modulesStorage.empty())
		SaveModuleStorage(m_modulesStorage.front()->data().moduleInfo, true);
	m_config.Save();
}

void DebuggerWindow2::ForEachToolWindow(const std::function<void(wxFrame&)>& fn)
{
	for (wxFrame* w : std::initializer_list<wxFrame*>{m_registers, m_dump, m_breakpoints, m_modules, m_symbols})
	{
		if (w != nullptr)
			fn(*w);
	}
}

void DebuggerWindow2::CreateToolBar()
{
	m_toolbar = new wxToolBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTB_HORIZONTAL | wxTB_NODIVIDER | wxTB_NOALIGN);
	m_toolbar->SetToolBitmapSize(wxSize(16, 16));
	const wxColour tint = ToolTint();

	m_toolbar->AddTool(ID_TOOL_GOTO, wxEmptyString, Icon(DEBUGGER_GOTO_png, tint), wxNullBitmap, wxITEM_NORMAL, _("GoTo (CTRL + G)"), "test", nullptr);
	m_toolbar->AddSeparator();
	m_toolbar->AddTool(ID_TOOL_BREAKPOINT, wxEmptyString, Icon(DEBUGGER_BP_RED_png, tint), wxNullBitmap, wxITEM_NORMAL, _("Toggle Breakpoint (F9)"), wxEmptyString, nullptr);
	m_toolbar->AddSeparator();

	m_breakIcon = Icon(DEBUGGER_PAUSE_png, tint);
	m_runIcon = Icon(DEBUGGER_PLAY_png, tint);
	m_toolbar->AddTool(ID_TOOL_RUN_BREAK, wxEmptyString, m_breakIcon, wxNullBitmap, wxITEM_NORMAL, _("Break (F5)"), wxEmptyString, nullptr);
	m_toolbar->AddTool(ID_TOOL_STEP_INTO, wxEmptyString, Icon(DEBUGGER_STEP_INTO_png, tint), wxNullBitmap, wxITEM_NORMAL, _("Step Into (F11)"), wxEmptyString, nullptr);
	m_toolbar->AddTool(ID_TOOL_STEP_OVER, wxEmptyString, Icon(DEBUGGER_STEP_OVER_png, tint), wxNullBitmap, wxITEM_NORMAL, _("Step Over (F10)"), wxEmptyString, nullptr);
	m_toolbar->AddSeparator();
	m_toolbar->Realize();

	m_toolbar->EnableTool(ID_TOOL_STEP_INTO, false);
	m_toolbar->EnableTool(ID_TOOL_STEP_OVER, false);
}

void DebuggerWindow2::ApplyTheme()
{
	const wxColour normal = ToolTint();
	const wxColour greyed = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
	auto retint = [&](int tool, const auto& png) {
		m_toolbar->SetToolNormalBitmap(tool, Icon(png, normal));
		m_toolbar->SetToolDisabledBitmap(tool, Icon(png, greyed));
	};
	retint(ID_TOOL_GOTO, DEBUGGER_GOTO_png);
	retint(ID_TOOL_BREAKPOINT, DEBUGGER_BP_RED_png);
	retint(ID_TOOL_STEP_INTO, DEBUGGER_STEP_INTO_png);
	retint(ID_TOOL_STEP_OVER, DEBUGGER_STEP_OVER_png);
	m_breakIcon = Icon(DEBUGGER_PAUSE_png, normal);
	m_runIcon = Icon(DEBUGGER_PLAY_png, normal);
	m_toolbar->SetToolNormalBitmap(ID_TOOL_RUN_BREAK, debugger_isTrapped() ? m_runIcon : m_breakIcon);
	m_moduleLabel->SetForegroundColour(wxSystemSettings::SelectLightDark(wxColour(0x88, 0x27, 0xAA), wxColour(0xDC, 0xA0, 0xF0)));
	m_toolbar->Refresh();
	Refresh();
}

void DebuggerWindow2::SetTrappedState(bool trapped)
{
	// while trapped the run/break tool resumes and stepping is possible
	m_toolbar->SetToolShortHelp(ID_TOOL_RUN_BREAK, trapped ? _("Run (F5)") : _("Break (F5)"));
	m_toolbar->SetToolNormalBitmap(ID_TOOL_RUN_BREAK, trapped ? m_runIcon : m_breakIcon);
	m_toolbar->EnableTool(ID_TOOL_STEP_INTO, trapped);
	m_toolbar->EnableTool(ID_TOOL_STEP_OVER, trapped);
}

void DebuggerWindow2::BuildMenus()
{
	const DebuggerConfig& cfg = m_config.data();
	auto* bar = new wxMenuBar();

	auto* fileMenu = new wxMenu();
	fileMenu->Append(ID_EXIT, _("&Exit"));
	bar->Append(fileMenu, _("&File"));

	auto* optionsMenu = new wxMenu();
	optionsMenu->AppendCheckItem(ID_OPT_PIN, _("&Pin to main window"))->Check(cfg.pin_to_main);
	optionsMenu->AppendCheckItem(ID_OPT_BREAK_ON_ENTRY, _("Break on &entry point"))->Check(cfg.break_on_start);
	optionsMenu->AppendCheckItem(ID_OPT_LOG_MEMORY_BPS, _("Log only memory breakpoints"))->Check(cfg.log_memory_breakpoints);
	optionsMenu->AppendCheckItem(ID_OPT_INTERPRETER, _("Switch to &interpreter CPU mode"));
	bar->Append(optionsMenu, _("&Options"));

	auto* windowMenu = new wxMenu();
	windowMenu->AppendCheckItem(ID_WIN_REGISTERS, _("&Registers"))->Check(cfg.show_register);
	windowMenu->AppendCheckItem(ID_WIN_DUMP, _("&Memory Dump"))->Check(cfg.show_dump);
	windowMenu->AppendCheckItem(ID_WIN_BREAKPOINTS, _("&Breakpoints"))->Check(cfg.show_breakpoints);
	windowMenu->AppendCheckItem(ID_WIN_MODULES, _("Module&list"))->Check(cfg.show_modules);
	windowMenu->AppendCheckItem(ID_WIN_SYMBOLS, _("&Symbols"))->Check(cfg.show_symbols);
	bar->Append(windowMenu, _("&Window"));

	SetMenuBar(bar);
}

void DebuggerWindow2::OnToolbar(wxCommandEvent& event)
{
	const int id = event.GetId();
	if (id == ID_TOOL_GOTO)
	{
		m_disasm->GoToAddressDialog();
		return;
	}
	if (id == ID_TOOL_RUN_BREAK)
	{
		if (debugger_isTrapped())
			debugger_stepCommand(DebuggerStepCommand::Run);
		else
			debugger_requestBreak();
		return;
	}
	if ((id == ID_TOOL_STEP_INTO || id == ID_TOOL_STEP_OVER) && debugger_isTrapped())
		debugger_stepCommand(id == ID_TOOL_STEP_INTO ? DebuggerStepCommand::StepInto : DebuggerStepCommand::StepOver);
}

void DebuggerWindow2::OnOptionsMenu(wxCommandEvent& event)
{
	DebuggerConfig& cfg = m_config.data();
	switch (event.GetId())
	{
	case ID_OPT_PIN:
		cfg.pin_to_main = !cfg.pin_to_main;
		if (cfg.pin_to_main)
			OnParentMove(m_mainPos, m_mainSize);
		break;
	case ID_OPT_BREAK_ON_ENTRY:
		cfg.break_on_start = !cfg.break_on_start;
		debugger_setOptionBreakOnEntry(cfg.break_on_start);
		break;
	case ID_OPT_LOG_MEMORY_BPS:
		cfg.log_memory_breakpoints = !cfg.log_memory_breakpoints;
		debugger_setOptionLogOnlyMemoryBreakpoints(cfg.log_memory_breakpoints);
		break;
	case ID_OPT_INTERPRETER:
		m_forceInterpreter = !m_forceInterpreter;
		SetInterpreterOnly(m_forceInterpreter);
		break;
	default:
		return;
	}
	m_config.Save();
}

void DebuggerWindow2::OnWindowMenu(wxCommandEvent& event)
{
	DebuggerConfig& cfg = m_config.data();
	bool* flag = nullptr;
	wxFrame* window = nullptr;
	switch (event.GetId())
	{
	case ID_WIN_REGISTERS: flag = &cfg.show_register; window = m_registers; break;
	case ID_WIN_DUMP: flag = &cfg.show_dump; window = m_dump; break;
	case ID_WIN_BREAKPOINTS: flag = &cfg.show_breakpoints; window = m_breakpoints; break;
	case ID_WIN_MODULES: flag = &cfg.show_modules; window = m_modules; break;
	case ID_WIN_SYMBOLS: flag = &cfg.show_symbols; window = m_symbols; break;
	default: return;
	}
	*flag = !*flag;
	window->Show(*flag);
	m_config.Save();
}

void DebuggerWindow2::OnShowEvent(wxShowEvent& event)
{
	ForEachToolWindow([](wxFrame& w) { w.Show(); });
	event.Skip();
}

void DebuggerWindow2::OnCloseEvent(wxCloseEvent& event)
{
	debugger_setOptionBreakOnEntry(false);
	wxPostEvent(m_parent, wxCloseEvent(wxEVT_DEBUGGER_CLOSE));
	event.Skip();
}

bool DebuggerWindow2::Show(bool show)
{
	const bool changed = wxFrame::Show(show);
	const DebuggerConfig& cfg = m_config.data();
	m_registers->Show(show && cfg.show_register);
	m_dump->Show(show && cfg.show_dump);
	m_breakpoints->Show(show && cfg.show_breakpoints);
	m_modules->Show(show && cfg.show_modules);
	m_symbols->Show(show && cfg.show_symbols);
	return changed;
}

void DebuggerWindow2::OnParentMove(const wxPoint& main_position, const wxSize& main_size)
{
	m_mainPos = main_position;
	m_mainSize = main_size;
	if (!m_config.data().pin_to_main)
		return;

	// directly above the main window, same width
	const int height = GetSize().GetHeight();
	SetSize(wxSize(main_size.x, height));
	SetPosition(wxPoint(main_position.x, main_position.y - height));

	m_registers->OnMainMove(main_position, main_size);
	m_dump->OnMainMove(main_position, main_size);
	m_breakpoints->OnMainMove(main_position, main_size);
	m_modules->OnMainMove(main_position, main_size);
	m_symbols->OnMainMove(main_position, main_size);
}

void DebuggerWindow2::ReloadModuleViews()
{
	m_modules->OnGameLoaded();
	m_symbols->OnGameLoaded();
	m_disasm->Init();
}

void DebuggerWindow2::OnGameLoaded()
{
	m_disasm->Init();
	m_dump->OnGameLoaded();
	m_modules->OnGameLoaded();
	m_breakpoints->OnGameLoaded();
	m_symbols->OnGameLoaded();

	if (RPLModule* main = RPLLoader_FindModuleByCodeAddr(MEMORY_CODEAREA_ADDR))
		m_moduleLabel->SetLabel(ModuleCaption(main->moduleName.c_str()));
	SendSizeEvent();
}

XMLDebuggerConfig& DebuggerWindow2::GetConfig()
{
	return m_config;
}

void DebuggerWindow2::ShowModuleOf(uint32 address)
{
	if (address == 0)
		address = m_disasm->GetViewBaseAddress();

	RPLModule* mod = RPLLoader_FindModuleByCodeAddr(address);
	if (mod != nullptr && mod->regionMappingBase_text.GetMPTR() != m_labelModuleBase)
	{
		m_moduleLabel->SetLabel(ModuleCaption(mod->moduleName.c_str()));
		m_labelModuleBase = mod->regionMappingBase_text.GetMPTR();
	}
	else if (address >= mmuRange_CODECAVE.getBase() && address < mmuRange_CODECAVE.getEnd())
	{
		m_moduleLabel->SetLabel(ModuleCaption("PixelU codecave"));
		m_labelModuleBase = mmuRange_CODECAVE.getBase();
	}
}

// ---------------------------------------------------------------------------
// DebuggerCallbacks: queue to the UI thread

void DebuggerWindow2::UpdateViewThreadsafe()
{
	wxQueueEvent(this, new wxCommandEvent(wxEVT_UPDATE_VIEW));
}

void DebuggerWindow2::NotifyDebugBreakpointHit()
{
	wxQueueEvent(this, new wxCommandEvent(wxEVT_BREAKPOINT_HIT));
}

void DebuggerWindow2::NotifyRun()
{
	wxQueueEvent(this, new wxCommandEvent(wxEVT_RUN));
}

void DebuggerWindow2::MoveToAddressInDisassembly(MPTR address)
{
	auto* evt = new wxCommandEvent(wxEVT_MOVE_TO_DISASM_ADDR);
	evt->SetClientData((void*)(uintptr_t)address);
	wxQueueEvent(this, evt);
}

void DebuggerWindow2::NotifyGraphicPacksModified()
{
	wxQueueEvent(this, new wxCommandEvent(wxEVT_NOTIFY_GRAPHIC_PACKS_MODIFIED));
}

void DebuggerWindow2::NotifyModuleLoaded(RPLModule* module)
{
	auto* evt = new wxCommandEvent(wxEVT_NOTIFY_MODULE_LOADED);
	evt->SetClientObject(new ModuleNotice(module));
	wxQueueEvent(this, evt);
}

void DebuggerWindow2::NotifyModuleUnloaded(RPLModule* module)
{
	auto* evt = new wxCommandEvent(wxEVT_NOTIFY_MODULE_UNLOADED);
	evt->SetClientObject(new ModuleNotice(module));
	wxQueueEvent(this, evt);
}
