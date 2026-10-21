# cubeb (ISC, Mozilla): audio output and microphone input. Built here from its
# sources with the two Windows backends, WASAPI and WinMM, and the speex resampler
# it bundles. (cubeb's own CMake project also builds tests, tools and Rust parts.)
cmake_minimum_required(VERSION 3.14)
project(cubeb VERSION 0.0.0 LANGUAGES C CXX)
set(src "${THIRD}/cubeb/cubeb")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Third-party libraries are linked statically")

add_library(cubeb
  "${src}/src/cubeb.c"
  "${src}/src/cubeb_log.cpp"
  "${src}/src/cubeb_mixer.cpp"
  "${src}/src/cubeb_resampler.cpp"
  "${src}/src/cubeb_strings.c"
  "${src}/src/cubeb_utils.cpp"
  "${src}/src/cubeb_wasapi.cpp"
  "${src}/src/cubeb_winmm.c"
)
add_library(cubeb::cubeb ALIAS cubeb)
target_include_directories(cubeb PUBLIC "$<BUILD_INTERFACE:${src}/include>")
target_compile_definitions(cubeb PRIVATE USE_WASAPI USE_WINMM)
pixelu_third_party_target(cubeb)

# cubeb_export.h, which cubeb.h includes.
include(GenerateExportHeader)
generate_export_header(cubeb EXPORT_FILE_NAME "${CMAKE_BINARY_DIR}/exports/cubeb_export.h")
target_include_directories(cubeb PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/exports>")

# The speex resampler cubeb ships, compiled into cubeb.
add_library(speex OBJECT "${src}/subprojects/speex/resample.c")
set_target_properties(speex PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
target_include_directories(speex INTERFACE "${src}/subprojects")
target_compile_definitions(speex PUBLIC OUTSIDE_SPEEX FLOATING_POINT EXPORT= RANDOM_PREFIX=speex)
pixelu_third_party_target(speex)
target_link_libraries(cubeb PRIVATE $<BUILD_INTERFACE:speex>)

find_package(Threads)
target_link_libraries(cubeb PRIVATE Threads::Threads avrt ole32 ksuser winmm)
