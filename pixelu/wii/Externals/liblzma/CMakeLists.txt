# liblzma from XZ Utils 5.2.4 (public domain), the copy in Dolphin's Externals:
# .xz/.lzma support for disc images (WIA/RVZ). config.h in this folder replaces
# the one XZ Utils' configure script would write.
project(lzma C)
set(src "${THIRD}/liblzma")

set(lzma_folders check common delta lz lzma rangecoder simple tuklib)
set(lzma_files "${src}/api/lzma.h")
file(GLOB lzma_api_headers "${src}/api/lzma/*.h")
list(APPEND lzma_files ${lzma_api_headers})
foreach(folder IN LISTS lzma_folders)
  file(GLOB folder_files "${src}/${folder}/*.c" "${src}/${folder}/*.h")
  # *_tablegen.c are stand-alone programs that print the tables in *_table.c.
  list(FILTER folder_files EXCLUDE REGEX "_tablegen\\.c$")
  list(APPEND lzma_files ${folder_files})
endforeach()

add_library(lzma STATIC ${lzma_files})
add_library(LibLZMA::LibLZMA ALIAS lzma)

target_compile_definitions(lzma
  PUBLIC LZMA_API_STATIC
  PRIVATE HAVE_CONFIG_H
)
if(MSVC)
  target_compile_definitions(lzma PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
endif()

set(private_includes "${CMAKE_CURRENT_SOURCE_DIR}")
foreach(folder IN LISTS lzma_folders)
  list(APPEND private_includes "${src}/${folder}")
endforeach()
target_include_directories(lzma
  PUBLIC "${src}/api"
  PRIVATE ${private_includes}
)
pixelu_third_party_target(lzma)
