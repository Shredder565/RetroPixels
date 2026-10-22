# The command-line tool (DolphinTool.exe): convert, verify, inspect and extract
# disc images, and run headless.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

add_executable(dolphin-tool)
pixelu_add_sources(dolphin-tool
  IN .
    ConvertCommand.cpp ConvertCommand.h DolphinTool.exe.manifest DolphinTool.rc
    ExtractCommand.cpp ExtractCommand.h HeaderCommand.cpp HeaderCommand.h
    ToolHeadlessPlatform.cpp ToolMain.cpp VerifyCommand.cpp VerifyCommand.h
)
set_target_properties(dolphin-tool PROPERTIES
  OUTPUT_NAME DolphinTool
  DEBUG_POSTFIX D
)

target_link_libraries(dolphin-tool PRIVATE
  discio
  uicommon
  cpp-optparse
  fmt::fmt
)

pixelu_use_pch(dolphin-tool)
