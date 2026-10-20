# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuResource: pictures and fonts compiled into the program. On Windows the
# icon font comes from cemu.rc (RCDATA); elsewhere a small assembler file
# includes the TTF with .incbin.

add_library(CemuResource)
pixelu_library(CemuResource)

enable_language(ASM)

pixelu_add_folder(CemuResource embedded resources.cpp resources.h)
pixelu_add_folder(CemuResource . CafeDefaultFont.cpp IconsFontAwesome5.h resource.h)

if(UNIX)
	if(APPLE)
		pixelu_add_folder(CemuResource embedded fontawesome_macos.S)
	else()
		pixelu_add_folder(CemuResource embedded fontawesome.S)
	endif()
endif()

target_include_directories(CemuResource PUBLIC "../")
target_link_libraries(CemuResource PRIVATE CemuCommon)
