# minizip-ng (Zlib licence): zip files (resource packs, NAND/SD exports). Its own
# CMake project, with only zlib, zstd and LZMA compression, all taken from the
# libraries built here, never from the system. Gives MINIZIP::minizip-ng.
set(MZ_COMPAT OFF)
set(MZ_BUILD_TESTS OFF)
set(MZ_BUILD_UNIT_TESTS OFF)
set(MZ_BUILD_FUZZ_TESTS OFF)
set(MZ_CODE_COVERAGE OFF)
set(MZ_FETCH_LIBS OFF)
set(MZ_BZIP2 OFF)
set(MZ_PKCRYPT OFF)
set(MZ_WZAES OFF)
set(MZ_OPENSSL OFF)
set(MZ_LIBBSD OFF)
set(SKIP_INSTALL_ALL ON)

# minizip-ng calls find_package() for its compression libraries; answer with ours.
set(CMAKE_DISABLE_FIND_PACKAGE_LibLZMA TRUE)
set(LIBLZMA_FOUND TRUE)
set(LIBLZMA_LIBRARIES LibLZMA::LibLZMA)
get_target_property(LIBLZMA_INCLUDE_DIRS LibLZMA::LibLZMA INTERFACE_INCLUDE_DIRECTORIES)

set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
set(ZLIB_FOUND TRUE)
set(ZLIB_LIBRARIES ZLIB::ZLIB)
set(ZLIB_INCLUDE_DIRS "$<TARGET_PROPERTY:ZLIB::ZLIB,INTERFACE_INCLUDE_DIRECTORIES>")

set(CMAKE_DISABLE_FIND_PACKAGE_ZSTD TRUE)
set(ZSTD_FOUND TRUE)
set(ZSTD_LIBRARIES zstd::zstd)
get_target_property(ZSTD_INCLUDE_DIRS zstd::zstd INTERFACE_INCLUDE_DIRECTORIES)

# No pkg-config lookups either.
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
set(PKGCONFIG_FOUND FALSE)

add_subdirectory("${THIRD}/minizip-ng/minizip-ng" "${CMAKE_CURRENT_BINARY_DIR}/minizip-ng")
pixelu_third_party_target(minizip-ng)
