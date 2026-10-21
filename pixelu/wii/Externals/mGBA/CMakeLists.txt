# mGBA (MPL-2.0): the Game Boy Advance emulated behind GBA link cables. mGBA's own
# CMake project in library-only mode, with LZMA and zlib from here.
# Gives mGBA::mgba.
set(LIBMGBA_ONLY ON)
set(USE_LZMA ON)
add_subdirectory("${THIRD}/mGBA/mgba" "${CMAKE_CURRENT_BINARY_DIR}/mgba" EXCLUDE_FROM_ALL)

target_compile_definitions(mgba PUBLIC HAVE_CRC32 ENABLE_VFS ENABLE_DIRECTORIES)
target_link_libraries(mgba ZLIB::ZLIB)
if(NOT MSVC)
  target_compile_options(mgba PRIVATE -Wno-unused-parameter -Wno-unused-result -Wno-unused-variable)
endif()
add_library(mGBA::mgba ALIAS mgba)
pixelu_third_party_target(mgba)
