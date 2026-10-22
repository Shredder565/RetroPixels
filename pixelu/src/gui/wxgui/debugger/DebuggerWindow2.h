// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "wxgui/debugger/DisasmCtrl.h"
#include "config/XMLConfig.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"

#include <wx/bitmap.h>
#include <wx/frame.h>
#include <wx/mstream.h>

class BreakpointWindow;
class RegisterWindow;
class DumpWindow;
class ModuleWindow;
class SymbolWindow;
class wxStaticText;
struct RPLStoredSymbol;

// events the debugger windows exchange; the cross-thread ones are queued by
// the DebuggerCallbacks overrides below
wxDECLARE_EVENT(wxEVT_UPDATE_VIEW, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_BREAKPOINT_HIT, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_RUN, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_BREAKPOINT_CHANGE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_MOVE_TO_DISASM_ADDR, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_NOTIFY_MODULE_LOADED, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_NOTIFY_MODULE_UNLOADED, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_NOTIFY_GRAPHIC_PACKS_MODIFIED, wxCommandEvent);

// the open debugger, or null
extern class DebuggerWindow2* s_debuggerWindow;

// debugger/config.xml
struct DebuggerConfig
{
	bool pin_to_main = true;
	bool break_on_start = true;
	bool log_memory_breakpoints = false;

	// which tool windows are visible
	bool show_register = true;
	bool show_dump = true;
	bool show_stack = true;
	bool show_breakpoints = true;
	bool show_modules = true;
	bool show_symbols = true;

	void Load(XMLConfigParser& parser);
	void Save(XMLConfigParser& parser);
};
typedef XMLDataConfig<DebuggerConfig> XMLDebuggerConfig;

// where a module's text and data sections live now and in the original file
struct DebuggerModuleInfo
{
	struct ModuleArea
	{
		MPTR base;
		MPTR origBase;
		uint32 size;

		bool ContainsAddress(MPTR addr) const { return addr >= base && addr - base < size; }
		bool ContainsOriginalAddress(MPTR addr) const { return addr >= origBase && addr - origBase < size; }
	};

	std::string moduleName;
	uint32 patchCRC;
	ModuleArea textArea;
	ModuleArea dataArea;

	DebuggerModuleInfo(RPLModule* module);
};

// per-module breakpoints and comments, stored module-relative in
// debugger/<module>_<crc>.xml, plus the symbols loaded from the .map file
struct DebuggerModuleStorage
{
	DebuggerModuleInfo moduleInfo;
	bool delete_breakpoints_after_saving;
	std::vector<RPLStoredSymbol*> loaded_map_symbols;

	void Load(XMLConfigParser& parser);
	void Save(XMLConfigParser& parser);
};
typedef XMLDataConfig<DebuggerModuleStorage> XMLDebuggerModuleConfig;

// decodes an embedded PNG and recolours its black pixels
static wxBitmap LoadThemedBitmapFromPNG(const uint8* data, size_t size, const wxColour& tint)
{
	wxMemoryInputStream input(data, size);
	wxImage picture(input, wxBITMAP_TYPE_PNG);
	picture.Replace(0, 0, 0, tint.Red(), tint.Green(), tint.Blue());
	return wxBitmap(picture);
}

// The PPC debugger: toolbar, module label and disassembly, owning the floating
// register, dump, breakpoint, module and symbol windows.
class DebuggerWindow2 : public wxFrame, public DebuggerCallbacks
{
public:
	void CreateToolBar();
	void LoadModuleStorage(const struct DebuggerModuleInfo& moduleInfo);
	void SaveModuleStorage(const struct DebuggerModuleInfo& moduleInfo, bool deleteModuleStorage);
	DebuggerWindow2(wxFrame& parent, const wxRect& display_size);
	~DebuggerWindow2();
	// saves and drops all module storage; safe to call more than once
	void CleanupForDestroy();

	void OnParentMove(const wxPoint& position, const wxSize& size);
	void OnGameLoaded();

	XMLDebuggerConfig& GetConfig();

	bool Show(bool show = true) override;
	std::wstring GetModuleStoragePath(std::string module_name, uint32_t crc_hash) const;

private:
	// DebuggerCallbacks, called from emulator threads
	void UpdateViewThreadsafe() override;
	void NotifyDebugBreakpointHit() override;
	void NotifyRun() override;
	void MoveToAddressInDisassembly(MPTR address) override;
	void NotifyGraphicPacksModified() override;
	void NotifyModuleLoaded(struct RPLModule* module) override;
	void NotifyModuleUnloaded(struct RPLModule* module) override;

	void BuildMenus();
	void ApplyTheme();
	void SetTrappedState(bool trapped);
	void ShowModuleOf(uint32 address = 0);
	void ReloadModuleViews();
	void ForEachToolWindow(const std::function<void(wxFrame&)>& fn);
	std::wstring GetModuleMapPath(std::string module_name, uint32_t crc_hash) const;
	void LoadModuleMap(DebuggerModuleStorage& storage);
	void UnloadModuleMap(DebuggerModuleStorage& storage);

	void OnToolbar(wxCommandEvent& event);
	void OnOptionsMenu(wxCommandEvent& event);
	void OnWindowMenu(wxCommandEvent& event);
	void OnShowEvent(wxShowEvent& event);
	void OnCloseEvent(wxCloseEvent& event);

	XMLDebuggerConfig m_config;
	std::vector<std::unique_ptr<XMLDebuggerModuleConfig>> m_modulesStorage;

	wxPoint m_mainPos;
	wxSize m_mainSize;

	RegisterWindow* m_registers = nullptr;
	DumpWindow* m_dump = nullptr;
	BreakpointWindow* m_breakpoints = nullptr;
	ModuleWindow* m_modules = nullptr;
	SymbolWindow* m_symbols = nullptr;
	DisasmCtrl* m_disasm = nullptr;

	wxToolBar* m_toolbar = nullptr;
	wxBitmap m_runIcon, m_breakIcon;
	wxStaticText* m_moduleLabel = nullptr;
	uint32 m_labelModuleBase = 0;
	bool m_forceInterpreter = false;
};
