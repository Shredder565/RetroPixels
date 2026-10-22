# ImPlot (MIT): the graphs in the on-screen performance display.
set(src "${THIRD}/implot/implot")

add_library(implot STATIC "${src}/implot.cpp" "${src}/implot_items.cpp")
target_include_directories(implot PUBLIC "${src}")
target_link_libraries(implot PRIVATE imgui fmt::fmt)
set(force_include "${CMAKE_CURRENT_SOURCE_DIR}/implot_force_include.h")
if(MSVC)
  target_compile_options(implot PRIVATE "/FI${force_include}")
else()
  target_compile_options(implot PRIVATE -include "${force_include}")
  set_target_properties(implot PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
pixelu_third_party_target(implot)
