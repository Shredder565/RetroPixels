# SPIRV-Cross (Apache-2.0): turns the SPIR-V that glslang makes into HLSL for the
# Direct3D 11 and 12 backends.
set(src "${THIRD}/spirv_cross/SPIRV-Cross")

set(parts cfg cpp cross cross_c cross_parsed_ir cross_util glsl hlsl msl parser reflect)
set(files "${src}/GLSL.std.450.h" "${src}/spirv.h" "${src}/spirv.hpp"
  "${src}/spirv_common.hpp" "${src}/spirv_cross_containers.hpp"
  "${src}/spirv_cross_error_handling.hpp" "${src}/spirv_cross_c.h")
foreach(part IN LISTS parts)
  list(APPEND files "${src}/spirv_${part}.cpp")
  if(NOT part STREQUAL "cross_c")
    list(APPEND files "${src}/spirv_${part}.hpp")
  endif()
endforeach()

add_library(spirv_cross STATIC ${files})
target_include_directories(spirv_cross PUBLIC "${src}/include" "${src}")
# Errors become assertions instead of C++ exceptions.
target_compile_definitions(spirv_cross PUBLIC SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS)
if(NOT MSVC)
  target_compile_options(spirv_cross PRIVATE -std=c++11)
endif()
pixelu_third_party_target(spirv_cross)
