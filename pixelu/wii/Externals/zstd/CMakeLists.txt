# Zstandard (BSD-3-Clause): disc image compression (RVZ). Its own CMake project,
# static library only. Gives zstd::zstd.
cmake_minimum_required(VERSION 3.13...4.2.0)

# Let the normal variables below decide zstd's option() values.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(ZSTD_BUILD_PROGRAMS OFF)
set(ZSTD_BUILD_SHARED OFF)
add_subdirectory("${THIRD}/zstd/zstd" "${CMAKE_CURRENT_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)

add_library(zstd::zstd ALIAS libzstd_static)
pixelu_third_party_target(libzstd_static)
