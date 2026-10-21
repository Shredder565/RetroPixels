# libcurl (curl licence): HTTP(S) for RetroAchievements, the netplay index, WC24
# and game update checks. curl's own CMake project as a static, HTTP-only library
# using Windows' Schannel for TLS, without compression or other extras.
set(BUILD_CURL_EXE OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_LIBCURL_DOCS OFF)
set(BUILD_MISC_DOCS OFF)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_STATIC_LIBS ON)
set(BUILD_TESTING OFF)
set(CURL_ENABLE_EXPORT_TARGET OFF)
set(ENABLE_CURL_MANUAL OFF)
set(HTTP_ONLY ON)
set(CURL_USE_SCHANNEL ON)
set(CURL_USE_LIBPSL OFF)
set(CURL_USE_LIBSSH2 OFF)
set(USE_LIBIDN2 OFF)
set(USE_NGHTTP2 OFF)

# No content compression. This curl version still looks zlib, zstd and brotli up
# when the *_FOUND variables are set, so clear those as well.
foreach(codec ZLIB ZSTD BROTLI)
  set(CURL_${codec} OFF CACHE BOOL "" FORCE)
  set(${codec}_FOUND FALSE)
endforeach()

add_subdirectory("${THIRD}/curl/curl" "${CMAKE_CURRENT_BINARY_DIR}/curl")
