# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
#
# Assembles the Sys folder next to PixelU-Wii.exe. Run as a script:
#
#   cmake -DTHIRD_PARTY_SYS=<Dolphin checkout>/Data/Sys -DOWN_SYS=<pixelu/wii>/Data/Sys
#         -DOUT=<Binaries>/Sys -P PixelUAssembleSys.cmake
#
# Sys is PixelU-Wii's own Data/Sys (pictures, theme, shaders, controller profiles, Wii NAND
# defaults, IPL fonts) plus the few files below that are still taken from the Dolphin checkout.
# Everything else in that checkout's Data/Sys is left out on purpose (Dolphin's themes and
# shaders, graphics mods, the debugger's symbol database, its NAND files and fonts), so a new
# file there never ships by accident.

foreach(var THIRD_PARTY_SYS OWN_SYS OUT)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "PixelUAssembleSys: ${var} is not set")
  endif()
endforeach()
foreach(dir "${THIRD_PARTY_SYS}" "${OWN_SYS}")
  if(NOT IS_DIRECTORY "${dir}")
    message(FATAL_ERROR "PixelUAssembleSys: ${dir} is not a folder")
  endif()
endforeach()

# What is still taken from the Dolphin checkout (relative to its Data/Sys; globs allowed).
set(from_third_party
  # Kept unchanged by the owner's decision (2026-10-21): per-game settings, their
  # RetroAchievements hashes.
  "GameSettings"
  "ApprovedInis.json"
  # GameTDB game title lists (third-party data, credited in COPYING).
  "wiitdb-*.txt"
  "triforcetdb-*.txt"
  # Bitstream Vera Sans Mono, the on-screen display font (third-party, with its licence).
  "Resources/OSD_Font.ttf"
  "Resources/OSD_Font_VeraMono_Copyright.txt"
  # Gecko code handler (third-party, MIT; credited in COPYING).
  "codehandler.bin"
  # Free DSP ROM replacement written by the Dolphin team (only used for DSP LLE without the
  # user's own dumps; see docs/b2-log.txt for why it is still Dolphin's).
  "GC/dsp_rom.bin"
  "GC/dsp_coef.bin"
)

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

set(copied 0)
foreach(pattern IN LISTS from_third_party)
  file(GLOB matches LIST_DIRECTORIES true RELATIVE "${THIRD_PARTY_SYS}" "${THIRD_PARTY_SYS}/${pattern}")
  if(NOT matches)
    message(FATAL_ERROR "PixelUAssembleSys: nothing matches ${THIRD_PARTY_SYS}/${pattern}")
  endif()
  foreach(rel IN LISTS matches)
    get_filename_component(parent "${OUT}/${rel}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    if(IS_DIRECTORY "${THIRD_PARTY_SYS}/${rel}")
      file(COPY "${THIRD_PARTY_SYS}/${rel}" DESTINATION "${parent}")
    else()
      file(COPY_FILE "${THIRD_PARTY_SYS}/${rel}" "${OUT}/${rel}")
    endif()
    math(EXPR copied "${copied} + 1")
  endforeach()
endforeach()

# PixelU-Wii's own files go over them (and may replace any of them).
file(COPY "${OWN_SYS}/" DESTINATION "${OUT}")

# The graphics mod folder is watched for changes even when nothing is in it.
file(MAKE_DIRECTORY "${OUT}/Load/GraphicMods")

message(STATUS "Assembled ${OUT}: ${copied} entries from ${THIRD_PARTY_SYS}, the rest from ${OWN_SYS}")
