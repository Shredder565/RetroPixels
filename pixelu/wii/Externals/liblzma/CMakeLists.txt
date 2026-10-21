# liblzma from XZ Utils 5.2.4 (0BSD / public domain), src/liblzma and the tuklib
# helpers in src/common of the XZ Utils repository: .xz/.lzma support for disc
# images (WIA/RVZ). config.h in this folder replaces the one XZ Utils' configure
# script would write.
project(lzma C)
set(src "${THIRD}/liblzma/xz/src/liblzma")
set(tuklib "${THIRD}/liblzma/xz/src/common")

set(lzma_folders check common delta lz lzma rangecoder simple)
set(lzma_files "${src}/api/lzma.h")
file(GLOB lzma_api_headers "${src}/api/lzma/*.h")
list(APPEND lzma_files ${lzma_api_headers})
foreach(folder IN LISTS lzma_folders)
  file(GLOB folder_files "${src}/${folder}/*.c" "${src}/${folder}/*.h")
  # *_tablegen.c are stand-alone programs that print the tables in *_table.c.
  list(FILTER folder_files EXCLUDE REGEX "_tablegen\\.c$")
  # crc*_small.c are the table-less alternatives to crc*_fast.c (same symbols),
  # and simple/<cpu>.c the BCJ filters, which config.h leaves out.
  list(FILTER folder_files EXCLUDE REGEX "/check/crc(32|64)_small\\.c$")
  list(FILTER folder_files EXCLUDE REGEX "/simple/(arm|armthumb|ia64|powerpc|sparc|x86)\\.c$")
  list(APPEND lzma_files ${folder_files})
endforeach()
# tuklib: the helpers liblzma shares with the xz tool (CPU count, RAM size, ...).
file(GLOB tuklib_files "${tuklib}/*.c" "${tuklib}/*.h")
list(APPEND lzma_files ${tuklib_files})

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
list(APPEND private_includes "${tuklib}")
target_include_directories(lzma
  PUBLIC "${src}/api"
  PRIVATE ${private_includes}
)
pixelu_third_party_target(lzma)
