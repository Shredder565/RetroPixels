# zlib-ng (Zlib licence) in zlib-compatible mode, as the zlib every other library
# here links: its own CMake project, static, no tests or install rules.
# Gives ZLIB::ZLIB; zlib.h and zconf.h are generated in its build folder.
set(ZLIB_ENABLE_TESTS OFF)
set(ZLIB_COMPAT ON)
set(SKIP_INSTALL_ALL ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Third-party libraries are linked statically")

add_subdirectory("${THIRD}/zlib-ng/zlib-ng" "${CMAKE_CURRENT_BINARY_DIR}/zlib-ng")

# For projects below that look zlib up with find_package(ZLIB).
set(ZLIB_INCLUDE_DIR "${CMAKE_BINARY_DIR}/zlib-ng" CACHE STRING "zlib include folder")
set(ZLIB_LIBRARY ZLIB::ZLIB CACHE STRING "zlib library")
add_library(ZLIB::ZLIB ALIAS zlib)
pixelu_third_party_target(zlib)
