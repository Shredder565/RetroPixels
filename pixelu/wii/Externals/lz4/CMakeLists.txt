# LZ4 (BSD-2-Clause): save state compression. lz4's own CMake project
# (build/cmake) in bundled mode: the static library only. Gives LZ4::LZ4.
cmake_minimum_required(VERSION 3.13)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(LZ4_BUILD_CLI OFF)
set(LZ4_BUILD_LEGACY_LZ4C OFF)
set(LZ4_BUNDLED_MODE ON)
add_subdirectory("${THIRD}/lz4/lz4/build/cmake" "${CMAKE_CURRENT_BINARY_DIR}/lz4/build/cmake")

# lz4 carries its own copy of xxHash; keep its symbols apart from the real xxHash.
target_compile_definitions(lz4_static PRIVATE XXH_NAMESPACE=LZ4_)
add_library(LZ4::LZ4 ALIAS lz4_static)
pixelu_third_party_target(lz4_static)
