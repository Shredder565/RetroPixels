# Dear ImGui (MIT): the on-screen display and the performance graphs.
set(src "${THIRD}/imgui/imgui")

add_library(imgui STATIC
  "${src}/imgui.cpp"
  "${src}/imgui_demo.cpp"
  "${src}/imgui_draw.cpp"
  "${src}/imgui_tables.cpp"
  "${src}/imgui_widgets.cpp"
)
# This folder has imgui_user_config.h; ${THIRD}/imgui allows <imgui/imgui.h>.
target_include_directories(imgui PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${THIRD}/imgui"
  "${src}"
)
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="imgui_user_config.h")
target_link_libraries(imgui PRIVATE fmt::fmt)
if(NOT MSVC)
  set_target_properties(imgui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
pixelu_third_party_target(imgui)
