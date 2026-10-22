# libcurl (curl licence): HTTP(S) for RetroAchievements, the netplay index, WC24
# and game update checks. curl's own CMake project as a static, HTTP-only library
# using Windows' Schannel for TLS (on Linux the Mbed TLS built here, with the
# system's certificates in /etc/ssl/certs), without compression or other extras.
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
if(WIN32)
  set(CURL_USE_SCHANNEL ON)
else()
  set(CURL_USE_OPENSSL OFF)
  set(CURL_USE_MBEDTLS ON)
  set(CURL_USE_PKGCONFIG OFF)
  set(MBEDTLS_INCLUDE_DIR "${THIRD}/mbedtls/include")
  set(MBEDTLS_LIBRARY mbedtls)
  set(MBEDX509_LIBRARY mbedx509)
  set(MBEDCRYPTO_LIBRARY mbedcrypto)
  # Every distribution has its CA certificates in (or linked from) /etc/ssl/certs;
  # Mbed TLS reads each file there, so a bundle file in it works as well.
  set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH "/etc/ssl/certs" CACHE STRING "" FORCE)
endif()
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
