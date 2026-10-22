# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuGui: the interface the emulator core uses to reach the user interface
# (gui/interface), plus the wxWidgets implementation of it

add_library(CemuGui INTERFACE)
target_include_directories(CemuGui INTERFACE "interface")

if(ENABLE_WXWIDGETS)
	add_subdirectory(wxgui)
	target_link_libraries(CemuGui INTERFACE CemuWxGui)
endif()
