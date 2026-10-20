# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuWxGui: the wxWidgets user interface

add_library(CemuWxGui STATIC)
pixelu_add_folder(CemuWxGui .
	AudioDebuggerWindow.cpp
	AudioDebuggerWindow.h
	CemuApp.cpp
	CemuApp.h
	CemuUpdateWindow.cpp
	CemuUpdateWindow.h
	ChecksumTool.cpp
	ChecksumTool.h
	DownloadCustomGraphicPackWindow.cpp
	DownloadCustomGraphicPackWindow.h
	DownloadGraphicPacksWindow.cpp
	DownloadGraphicPacksWindow.h
	GameProfileWindow.cpp
	GameProfileWindow.h
	GameUpdateWindow.cpp
	GameUpdateWindow.h
	GeneralSettings2.cpp
	GeneralSettings2.h
	GettingStartedDialog.cpp
	GettingStartedDialog.h
	GraphicPacksWindow2.cpp
	GraphicPacksWindow2.h
	LoggingWindow.cpp
	LoggingWindow.h
	MainWindow.cpp
	MainWindow.h
	MemorySearcherTool.cpp
	MemorySearcherTool.h
	PadViewFrame.cpp
	PadViewFrame.h
	RPEmbed.cpp
	RPEmbed.h
	TitleManager.cpp
	TitleManager.h
	wxCemuConfig.cpp
	wxCemuConfig.h
	wxgui.h
	wxHelper.h
	wxWindowSystem.cpp
)
pixelu_add_folder(CemuWxGui canvas IRenderCanvas.h)
pixelu_add_folder(CemuWxGui components
	TextList.cpp
	TextList.h
	wxDownloadManagerList.cpp
	wxDownloadManagerList.h
	wxGameList.cpp
	wxGameList.h
	wxInputDraw.cpp
	wxInputDraw.h
	wxLogCtrl.cpp
	wxLogCtrl.h
	wxProgressDialogManager.h
	wxTitleManagerList.cpp
	wxTitleManagerList.h
)
pixelu_add_folder(CemuWxGui debugger
	BreakpointWindow.cpp
	BreakpointWindow.h
	DebuggerWindow2.cpp
	DebuggerWindow2.h
	DisasmCtrl.cpp
	DisasmCtrl.h
	DumpCtrl.cpp
	DumpCtrl.h
	DumpWindow.cpp
	DumpWindow.h
	ModuleWindow.cpp
	ModuleWindow.h
	RegisterWindow.cpp
	RegisterWindow.h
	SymbolCtrl.cpp
	SymbolCtrl.h
	SymbolWindow.cpp
	SymbolWindow.h
)
pixelu_add_folder(CemuWxGui dialogs/CreateAccount wxCreateAccountDialog.cpp wxCreateAccountDialog.h)
pixelu_add_folder(CemuWxGui dialogs/SaveImport
	SaveImportWindow.cpp
	SaveImportWindow.h
	SaveTransfer.cpp
	SaveTransfer.h
)
pixelu_add_folder(CemuWxGui EmulatedUSBDevices EmulatedUSBDeviceFrame.cpp EmulatedUSBDeviceFrame.h)
pixelu_add_folder(CemuWxGui helpers
	wxControlObject.h
	wxCustomData.h
	wxCustomEvents.cpp
	wxCustomEvents.h
	wxHelpers.cpp
	wxHelpers.h
	wxLogEvent.h
	wxWayland.cpp
	wxWayland.h
)
pixelu_add_folder(CemuWxGui input
	HotkeySettings.cpp
	HotkeySettings.h
	InputAPIAddWindow.cpp
	InputAPIAddWindow.h
	InputSettings2.cpp
	InputSettings2.h
)
pixelu_add_folder(CemuWxGui input/panels
	ClassicControllerInputPanel.cpp
	ClassicControllerInputPanel.h
	InputPanel.cpp
	InputPanel.h
	ProControllerInputPanel.cpp
	ProControllerInputPanel.h
	VPADInputPanel.cpp
	VPADInputPanel.h
	WiimoteInputPanel.cpp
	WiimoteInputPanel.h
)
pixelu_add_folder(CemuWxGui input/settings
	DefaultControllerSettings.cpp
	DefaultControllerSettings.h
	WiimoteControllerSettings.cpp
	WiimoteControllerSettings.h
)
pixelu_add_folder(CemuWxGui windows/PPCThreadsViewer
	DebugPPCThreadsWindow.cpp
	DebugPPCThreadsWindow.h
)
pixelu_add_folder(CemuWxGui windows/TextureRelationViewer
	TextureRelationWindow.cpp
	TextureRelationWindow.h
)
pixelu_add_folder(CemuWxGui wxcomponents checktree.cpp checktree.h)

# render canvases of the enabled backends
if(ENABLE_OPENGL)
	pixelu_add_folder(CemuWxGui canvas OpenGLCanvas.cpp OpenGLCanvas.h)
endif()
if(ENABLE_VULKAN)
	pixelu_add_folder(CemuWxGui canvas VulkanCanvas.cpp VulkanCanvas.h)
endif()
if(ENABLE_BLUEZ)
	pixelu_add_folder(CemuWxGui input PairingDialog.cpp PairingDialog.h)
endif()

pixelu_library(CemuWxGui)

target_include_directories(CemuWxGui PUBLIC "../")
# public: ChecksumTool.h includes rapidjson
target_include_directories(CemuWxGui PUBLIC ${RAPIDJSON_INCLUDE_DIRS})

target_link_libraries(CemuWxGui
	PRIVATE
		CemuCommon
		CemuResource
		libzip::zip
		ZArchive::zarchive
		CemuComponents
		pugixml::pugixml
		CemuCafe
	PUBLIC
		CURL::libcurl
)

if(ENABLE_WXWIDGETS AND UNIX AND NOT APPLE)
	# public: guiWrapper.h includes the GDK key symbols
	target_link_libraries(CemuWxGui PUBLIC GTK3::gtk)
	if(ENABLE_WAYLAND)
		target_link_libraries(CemuWxGui PRIVATE Wayland::Client CemuWaylandProtocols)
	endif()
endif()

if(ENABLE_CUBEB)
	target_link_libraries(CemuWxGui PRIVATE cubeb::cubeb)
endif()
if(ENABLE_SDL)
	target_link_libraries(CemuWxGui PRIVATE SDL3::SDL3)
endif()
if(UNIX AND NOT APPLE AND ENABLE_FERAL_GAMEMODE)
	target_link_libraries(CemuWxGui PRIVATE gamemode)
endif()
if(ENABLE_WXWIDGETS)
	# public: CemuApp.h includes wx/app.h
	target_link_libraries(CemuWxGui PUBLIC wxWidgets::wxWidgets)
endif()
if(WIN32)
	target_link_libraries(CemuWxGui PRIVATE bthprops) # Bluetooth pairing dialog
endif()

if(ALLOW_PORTABLE)
	target_compile_definitions(CemuWxGui PRIVATE CEMU_ALLOW_PORTABLE)
endif()
