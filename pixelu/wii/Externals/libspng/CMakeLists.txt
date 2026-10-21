# libspng (BSD-2-Clause): PNG reading and writing (screenshots, custom textures).
# Its own CMake project, static library only, no install rules. Gives spng::spng.
set(SPNG_SHARED OFF CACHE BOOL "libspng: no DLL")
set(SPNG_STATIC ON CACHE BOOL "libspng: static library")
set(BUILD_EXAMPLES OFF CACHE BOOL "libspng: no example programs")

set(saved_skip_install "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES TRUE)
add_subdirectory("${THIRD}/libspng/libspng" "${CMAKE_CURRENT_BINARY_DIR}/libspng")
set(CMAKE_SKIP_INSTALL_RULES "${saved_skip_install}")

add_library(spng::spng ALIAS spng_static)
