# PixelU-Wii build helpers. Original code for RetroPixels (PixelU-Wii).

# pixelu_add_sources(<target> [<file>...] [IN <folder> <file>...]...)
#
# Adds private sources to <target>. File names that follow "IN <folder>" are taken
# relative to that folder of the current source directory, until the next IN.
# "IN ." goes back to the directory itself.
function(pixelu_add_sources target)
  set(folder "")
  set(expect_folder FALSE)
  set(files)
  foreach(item IN LISTS ARGN)
    if(expect_folder)
      if(item STREQUAL ".")
        set(folder "")
      else()
        set(folder "${item}/")
      endif()
      set(expect_folder FALSE)
    elseif(item STREQUAL "IN")
      set(expect_folder TRUE)
    else()
      list(APPEND files "${folder}${item}")
    endif()
  endforeach()
  if(expect_folder)
    message(FATAL_ERROR "pixelu_add_sources(${target}): IN without a folder")
  endif()
  target_sources(${target} PRIVATE ${files})
endfunction()

# pixelu_library(<name> <sources as for pixelu_add_sources>)
#
# A library of PixelU-Wii's own code (static unless BUILD_SHARED_LIBS says otherwise).
function(pixelu_library name)
  add_library(${name})
  pixelu_add_sources(${name} ${ARGN})
endfunction()

# pixelu_use_pch(<target>)
#
# With MSVC, <target> compiles against the one precompiled header that Source/PCH
# builds for all of PixelU-Wii's code. Called after the target's main libraries,
# so their include folders come before the PCH's.
function(pixelu_use_pch target)
  if(MSVC)
    target_link_libraries(${target} PRIVATE use_pch)
  endif()
endfunction()

# pixelu_drop_option(<target> <option>)
#
# Takes <option> out of the compile options <target> got from its directory.
function(pixelu_drop_option target option)
  get_target_property(options ${target} COMPILE_OPTIONS)
  if(options)
    list(REMOVE_ITEM options "${option}")
    set_property(TARGET ${target} PROPERTY COMPILE_OPTIONS "${options}")
  endif()
endfunction()

# pixelu_third_party_target(<target>...)
#
# Third-party code is built as its authors wrote it: none of the project's warning
# options (/W4, /WX, /w1..., -W...) apply to it, and its own warnings are switched off.
function(pixelu_third_party_target)
  foreach(target IN LISTS ARGN)
    get_target_property(options ${target} COMPILE_OPTIONS)
    if(options)
      list(FILTER options EXCLUDE REGEX "^[-/][Ww]")
      set_property(TARGET ${target} PROPERTY COMPILE_OPTIONS "${options}")
    endif()
    if(MSVC)
      target_compile_options(${target} PRIVATE /W0)
    else()
      target_compile_options(${target} PRIVATE -w)
    endif()
  endforeach()
endfunction()

# pixelu_after_build_link(<target> <from> <to>)
#
# After <target> is built, makes <to> point at <from> (a symbolic link where the
# system allows one, otherwise a copy).
function(pixelu_after_build_link target from to)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DFROM=${from}" "-DTO=${to}"
            -P "${PIXELU_WII_CMAKE_DIR}/PixelULinkOrCopy.cmake"
    VERBATIM)
endfunction()
