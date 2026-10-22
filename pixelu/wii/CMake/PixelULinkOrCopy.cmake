# Original code for RetroPixels (PixelU-Wii).
#
# cmake -DFROM=<existing file or folder> -DTO=<new path> -P PixelULinkOrCopy.cmake
#
# Puts a symbolic link at TO that points at FROM. Windows only allows that with
# Developer Mode or as administrator, so when it fails TO becomes a copy instead.

if(NOT FROM OR NOT TO)
  message(FATAL_ERROR "PixelULinkOrCopy: give both FROM and TO")
endif()
if(NOT EXISTS "${FROM}")
  message(FATAL_ERROR "PixelULinkOrCopy: ${FROM} does not exist")
endif()

file(CREATE_LINK "${FROM}" "${TO}" RESULT link_result SYMBOLIC)
if(link_result EQUAL 0)
  return()
endif()

message(STATUS "Could not link ${TO} (${link_result}); copying ${FROM} instead")
file(REMOVE_RECURSE "${TO}")
if(IS_DIRECTORY "${FROM}")
  file(COPY "${FROM}/" DESTINATION "${TO}")
else()
  file(COPY_FILE "${FROM}" "${TO}" ONLY_IF_DIFFERENT)
endif()
