# glslang (BSD-3-Clause and others): GLSL to SPIR-V for the Vulkan and Direct3D
# backends. Only the library is built: no tools, tests, HLSL front end, optimiser
# or install rules.
set(src "${THIRD}/glslang/glslang")

set(ENABLE_PCH OFF)
set(BUILD_EXTERNAL OFF)
set(SKIP_GLSLANG_INSTALL ON)
set(ENABLE_GLSLANG_BINARIES OFF)
set(ENABLE_HLSL OFF)
set(ENABLE_OPT OFF)
set(ENABLE_CTEST OFF)
set(ENABLE_EXCEPTIONS OFF)
add_subdirectory("${src}" "${CMAKE_CURRENT_BINARY_DIR}/glslang")

if(TARGET glslang)
  # Users include <glslang/Public/ShaderLang.h> and <SPIRV/GlslangToSpv.h>.
  target_include_directories(glslang
    SYSTEM PUBLIC "$<BUILD_INTERFACE:${src}>" "$<BUILD_INTERFACE:${src}/Public>"
    PUBLIC "${THIRD}/glslang")
  pixelu_third_party_target(glslang)
endif()
