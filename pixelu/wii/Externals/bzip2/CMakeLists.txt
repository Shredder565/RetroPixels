# libbzip2 (bzip2 licence): .bz2 support for disc images (WIA). Built from its
# library sources; bzip2's own CMake also builds the tools and tests.
project(bzip2 C)
set(src "${THIRD}/bzip2/bzip2")

include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckTypeSize)

# The same platform answers bzip2's build normally works out.
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(stddef.h HAVE_STDDEF_H)
set(CMAKE_REQUIRED_DEFINITIONS -D_LARGEFILE64_SOURCE=1)
foreach(have HAVE_SYS_TYPES_H HAVE_STDINT_H HAVE_STDDEF_H)
  if(${have})
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -D${have})
  endif()
endforeach()
check_type_size(off64_t OFF64_T)
unset(CMAKE_REQUIRED_DEFINITIONS)
check_function_exists(fseeko HAVE_FSEEKO)
check_include_file(unistd.h HAVE_UNISTD_H)

add_library(bzip2 STATIC
  "${src}/blocksort.c"
  "${src}/bzlib.c"
  "${src}/compress.c"
  "${src}/crctable.c"
  "${src}/decompress.c"
  "${src}/huffman.c"
  "${src}/randtable.c"
  "${src}/bzlib.h"
  "${src}/bzlib_private.h"
)
add_library(BZip2::BZip2 ALIAS bzip2)
target_include_directories(bzip2 PUBLIC "${src}")

target_compile_definitions(bzip2 PRIVATE
  $<$<BOOL:${HAVE_OFF64_T}>:_LARGEFILE64_SOURCE=1>
  $<$<NOT:$<BOOL:${HAVE_FSEEKO}>>:NO_FSEEKO>
  $<$<BOOL:${HAVE_UNISTD_H}>:HAVE_UNISTD_H>
)
if(MSVC)
  target_compile_definitions(bzip2 PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
endif()
pixelu_third_party_target(bzip2)
