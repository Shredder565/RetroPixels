# Original code for RetroPixels (PixelU-Wii).
#
# cmake -DJSON_FILE=<ApprovedInis.json> -DTEMPLATE_FILE=<header.in> -DOUTPUT_FILE=<header>
#       -P PixelUAchievementHash.cmake
#
# Run at build time (from a custom command), so a changed list of approved game
# INIs is picked up without configuring again. The template gets the list's SHA-1
# as @ACHIEVEMENT_APPROVED_LIST_HASH@.

foreach(var JSON_FILE TEMPLATE_FILE OUTPUT_FILE)
  if(NOT ${var})
    message(FATAL_ERROR "PixelUAchievementHash: ${var} is not set")
  endif()
endforeach()

file(SHA1 "${JSON_FILE}" ACHIEVEMENT_APPROVED_LIST_HASH)
configure_file("${TEMPLATE_FILE}" "${OUTPUT_FILE}" @ONLY)
