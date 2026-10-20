# Original code for RetroPixels (PixelU). Not derived from Cemu.
# imguiImpl: the on-screen overlay. Dear ImGui itself (dependencies/imgui) is
# compiled into this library together with PixelU's renderer backends.

add_library(imguiImpl)
pixelu_add_folder(imguiImpl . imgui_extension.cpp imgui_extension.h)

if(ENABLE_OPENGL)
	pixelu_add_folder(imguiImpl . imgui_impl_opengl3.cpp imgui_impl_opengl3.h)
endif()
if(ENABLE_VULKAN)
	pixelu_add_folder(imguiImpl . imgui_impl_vulkan.cpp imgui_impl_vulkan.h)
endif()

pixelu_library(imguiImpl)

target_include_directories(imguiImpl PUBLIC "../")

set(PIXELU_IMGUI_DIR "../../dependencies/imgui")
pixelu_add_folder(imguiImpl ${PIXELU_IMGUI_DIR} imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp)
target_include_directories(imguiImpl PUBLIC "${PIXELU_IMGUI_DIR}/")

target_link_libraries(imguiImpl PRIVATE CemuCommon CemuGui)
