# The Qt front end: PixelU-Wii.exe (PixelU-Wii on Linux).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

# Qt 6.8.3's official MSVC x64 build (PIXELU_WII_QT_DIR, installed with
# aqtinstall). Tools such as moc and windeployqt run from the host's copy,
# which differs on an ARM64 host.
set(DOLPHIN_QT_ROOT "${PIXELU_WII_QT_DIR}")
set(DOLPHIN_QT_TARGET_SPEC "win32-msvc")
if(QT_HOST_PATH)
  set(DOLPHIN_QT_HOST_ROOT "${QT_HOST_PATH}")
else()
  set(DOLPHIN_QT_HOST_ROOT "${DOLPHIN_QT_ROOT}")
endif()
list(APPEND CMAKE_PREFIX_PATH "${DOLPHIN_QT_ROOT}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)
message(STATUS "Qt ${Qt6_VERSION}")
# Qt 6.10 and later keep qplatformnativeinterface.h (MainWindow.cpp) in GuiPrivate.
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.10.0)
  find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
  set(qt_gui_private Qt6::GuiPrivate)
else()
  set(qt_gui_private)
endif()
# The C++ standard is set for the whole project; Qt must not ask for another.
set_property(TARGET Qt6::Core PROPERTY INTERFACE_COMPILE_FEATURES "")

add_executable(dolphin-emu)
pixelu_add_sources(dolphin-emu
  IN .
    AboutDialog.cpp AboutDialog.h CheatSearchFactoryWidget.cpp CheatSearchFactoryWidget.h
    CheatSearchWidget.cpp CheatSearchWidget.h CheatsManager.cpp CheatsManager.h
    ConvertDialog.cpp ConvertDialog.h DiscordHandler.cpp DiscordHandler.h
    DiscordJoinRequestDialog.cpp DiscordJoinRequestDialog.h DolphinQt.manifest DolphinQt.rc
    GameCount.cpp GameCount.h GCMemcardCreateNewDialog.cpp GCMemcardCreateNewDialog.h
    GCMemcardManager.cpp GCMemcardManager.h Host.cpp Host.h HotkeyScheduler.cpp
    HotkeyScheduler.h Main.cpp MainWindow.cpp MainWindow.h MenuBar.cpp MenuBar.h
    NANDRepairDialog.cpp NANDRepairDialog.h NKitWarningDialog.cpp NKitWarningDialog.h
    RenderWidget.cpp RenderWidget.h ResourcePackManager.cpp ResourcePackManager.h Resources.cpp
    Resources.h RiivolutionBootWidget.cpp RiivolutionBootWidget.h SearchBar.cpp SearchBar.h
    Settings.cpp Settings.h ToolBar.cpp ToolBar.h Translation.cpp Translation.h Updater.cpp
    Updater.h WiiUpdate.cpp WiiUpdate.h
  IN Achievements
    AchievementBox.cpp AchievementBox.h AchievementHeaderWidget.cpp AchievementHeaderWidget.h
    AchievementLeaderboardWidget.cpp AchievementLeaderboardWidget.h
    AchievementProgressWidget.cpp AchievementProgressWidget.h AchievementSettingsWidget.cpp
    AchievementSettingsWidget.h AchievementsWindow.cpp AchievementsWindow.h
  IN Config
    ARCodeWidget.cpp ARCodeWidget.h CheatCodeEditor.cpp CheatCodeEditor.h
    CheatWarningWidget.cpp CheatWarningWidget.h CommonControllersWidget.cpp
    CommonControllersWidget.h ControllersPane.cpp ControllersPane.h FilesystemWidget.cpp
    FilesystemWidget.h FreeLookWidget.cpp FreeLookWidget.h FreeLookWindow.cpp FreeLookWindow.h
    GameConfigEdit.cpp GameConfigEdit.h GameConfigHighlighter.cpp GameConfigHighlighter.h
    GameConfigWidget.cpp GameConfigWidget.h GamecubeControllersWidget.cpp
    GamecubeControllersWidget.h GeckoCodeWidget.cpp GeckoCodeWidget.h GraphicsModListWidget.cpp
    GraphicsModListWidget.h GraphicsModWarningWidget.cpp GraphicsModWarningWidget.h
    HardcoreWarningWidget.cpp HardcoreWarningWidget.h InfoWidget.cpp InfoWidget.h
    LogConfigWidget.cpp LogConfigWidget.h LogWidget.cpp LogWidget.h NewPatchDialog.cpp
    NewPatchDialog.h PatchesWidget.cpp PatchesWidget.h PropertiesDialog.cpp PropertiesDialog.h
    SettingsWindow.cpp SettingsWindow.h VerifyWidget.cpp VerifyWidget.h
    WiimoteControllersWidget.cpp WiimoteControllersWidget.h
  IN Config/ConfigControls
    ConfigBool.cpp ConfigBool.h ConfigChoice.cpp ConfigChoice.h ConfigControl.h
    ConfigFloatSlider.cpp ConfigFloatSlider.h ConfigInteger.cpp ConfigInteger.h ConfigRadio.cpp
    ConfigRadio.h ConfigSlider.cpp ConfigSlider.h ConfigText.cpp ConfigText.h
    ConfigUserPath.cpp ConfigUserPath.h
  IN Config/ControllerInterface
    ControllerInterfaceWindow.cpp ControllerInterfaceWindow.h
    DualShockUDPClientEditServerDialog.cpp DualShockUDPClientEditServerDialog.h
    DualShockUDPClientWidget.cpp DualShockUDPClientWidget.h DualShockUDPSettings.cpp
    DualShockUDPSettings.h ServerStringValidator.cpp ServerStringValidator.h
  IN Config/Graphics
    AdvancedWidget.cpp AdvancedWidget.h ColorCorrectionConfigWindow.cpp
    ColorCorrectionConfigWindow.h EnhancementsWidget.cpp EnhancementsWidget.h GeneralWidget.cpp
    GeneralWidget.h GraphicsPane.cpp GraphicsPane.h HacksWidget.cpp HacksWidget.h
    PostProcessingConfigWindow.cpp PostProcessingConfigWindow.h
  IN Config/Mapping
    FreeLookGeneral.cpp FreeLookGeneral.h FreeLookRotation.cpp FreeLookRotation.h GBAPadEmu.cpp
    GBAPadEmu.h GCKeyboardEmu.cpp GCKeyboardEmu.h GCMicrophone.cpp GCMicrophone.h GCPadEmu.cpp
    GCPadEmu.h GCPadWiiUConfigDialog.cpp GCPadWiiUConfigDialog.h Hotkey3D.cpp Hotkey3D.h
    HotkeyControllerProfile.cpp HotkeyControllerProfile.h HotkeyDebugging.cpp HotkeyDebugging.h
    HotkeyGBA.cpp HotkeyGBA.h HotkeyGeneral.cpp HotkeyGeneral.h HotkeyGraphics.cpp
    HotkeyGraphics.h HotkeyStates.cpp HotkeyStates.h HotkeyStatesOther.cpp HotkeyStatesOther.h
    HotkeyTAS.cpp HotkeyTAS.h HotkeyUSBEmu.cpp HotkeyUSBEmu.h HotkeyWii.cpp HotkeyWii.h
    IOWindow.cpp MappingButton.cpp MappingButton.h MappingCommon.cpp MappingCommon.h
    MappingIndicator.cpp MappingIndicator.h MappingNumeric.cpp MappingNumeric.h
    MappingWidget.cpp MappingWidget.h MappingWindow.cpp MappingWindow.h WiimoteEmuExtension.cpp
    WiimoteEmuExtension.h WiimoteEmuExtensionMotionInput.cpp WiimoteEmuExtensionMotionInput.h
    WiimoteEmuExtensionMotionSimulation.cpp WiimoteEmuExtensionMotionSimulation.h
    WiimoteEmuGeneral.cpp WiimoteEmuGeneral.h WiimoteEmuMotionControl.cpp
    WiimoteEmuMotionControl.h WiimoteEmuMotionControlIMU.cpp WiimoteEmuMotionControlIMU.h
  IN Config/SDLHints
    SDLHintsWindow.cpp SDLHintsWindow.h
  IN Config/ToolTipControls
    BalloonTip.cpp BalloonTip.h ToolTipCheckBox.cpp ToolTipCheckBox.h ToolTipComboBox.cpp
    ToolTipComboBox.h ToolTipPushButton.cpp ToolTipPushButton.h ToolTipRadioButton.cpp
    ToolTipRadioButton.h ToolTipSlider.cpp ToolTipSlider.h ToolTipSpinBox.cpp ToolTipSpinBox.h
    ToolTipWidget.h
  IN Debugger
    AssembleInstructionDialog.cpp AssembleInstructionDialog.h AssemblerWidget.cpp
    AssemblerWidget.h AssemblyEditor.cpp AssemblyEditor.h BranchWatchDialog.cpp
    BranchWatchDialog.h BranchWatchTableModel.cpp BranchWatchTableModel.h BreakpointDialog.cpp
    BreakpointDialog.h BreakpointWidget.cpp BreakpointWidget.h CodeViewWidget.cpp
    CodeViewWidget.h CodeWidget.cpp CodeWidget.h EditSymbolDialog.cpp EditSymbolDialog.h
    GekkoSyntaxHighlight.cpp GekkoSyntaxHighlight.h JitBlockTableModel.cpp JitBlockTableModel.h
    JITWidget.cpp JITWidget.h MemoryViewWidget.cpp MemoryViewWidget.h MemoryWidget.cpp
    MemoryWidget.h NetworkWidget.cpp NetworkWidget.h PatchInstructionDialog.cpp
    PatchInstructionDialog.h RegisterColumn.cpp RegisterColumn.h RegisterWidget.cpp
    RegisterWidget.h ThreadWidget.cpp ThreadWidget.h WatchWidget.cpp WatchWidget.h
  IN EmulatedUSB
    LogitechMicWindow.cpp LogitechMicWindow.h WiiSpeakWindow.cpp WiiSpeakWindow.h
  IN FIFO
    FIFOAnalyzer.cpp FIFOAnalyzer.h FIFOPlayerWindow.cpp FIFOPlayerWindow.h
  IN GameList
    GameList.cpp GameList.h GameListModel.cpp GameListModel.h GameTracker.cpp GameTracker.h
    GridProxyModel.cpp GridProxyModel.h ListProxyModel.cpp ListProxyModel.h
  IN InfinityBase
    InfinityBaseWindow.cpp InfinityBaseWindow.h
  IN NetPlay
    ChunkedProgressDialog.cpp ChunkedProgressDialog.h ClickBlurLabel.cpp ClickBlurLabel.h
    GameDigestDialog.cpp GameDigestDialog.h GameListDialog.cpp GameListDialog.h
    NetPlayBrowser.cpp NetPlayBrowser.h NetPlayDialog.cpp NetPlayDialog.h
    NetPlaySetupDialog.cpp NetPlaySetupDialog.h PadMappingDialog.cpp PadMappingDialog.h
  IN QtUtils
    AnalyticsPrompt.cpp AnalyticsPrompt.h AspectRatioWidget.cpp AspectRatioWidget.h
    BlockUserInputFilter.cpp BlockUserInputFilter.h ClearLayoutRecursively.cpp
    ClearLayoutRecursively.h ClickableStatusBar.h DolphinFileDialog.cpp DolphinFileDialog.h
    DoubleClickEventFilter.cpp DoubleClickEventFilter.h ElidedButton.cpp ElidedButton.h
    FileOpenEventFilter.cpp FileOpenEventFilter.h FromStdString.h ImageConverter.cpp
    ImageConverter.h ModalMessageBox.cpp ModalMessageBox.h NonAutodismissibleMenu.cpp
    NonAutodismissibleMenu.h NonDefaultQPushButton.cpp NonDefaultQPushButton.h
    ParallelProgressDialog.h PartiallyClosableTabWidget.cpp PartiallyClosableTabWidget.h
    QtUtils.cpp QtUtils.h SetWindowDecorations.cpp SetWindowDecorations.h SignalBlocking.h
    UTF8CodePointCountValidator.cpp UTF8CodePointCountValidator.h
    WindowActivationEventFilter.cpp WindowActivationEventFilter.h WrapInScrollArea.cpp
    WrapInScrollArea.h
  IN Settings
    AdvancedPane.cpp AdvancedPane.h AudioPane.cpp AudioPane.h
    BroadbandAdapterSettingsDialog.cpp BroadbandAdapterSettingsDialog.h GameCubePane.cpp
    GameCubePane.h GeneralPane.cpp GeneralPane.h InterfacePane.cpp InterfacePane.h
    OnScreenDisplayPane.cpp OnScreenDisplayPane.h PathPane.cpp PathPane.h TriforcePane.cpp
    TriforcePane.h USBDevicePicker.cpp USBDevicePicker.h WiiPane.cpp WiiPane.h
  IN SkylanderPortal
    SkylanderModifyDialog.cpp SkylanderModifyDialog.h SkylanderPortalWindow.cpp
    SkylanderPortalWindow.h
  IN Styles/Dark
    dark.qrc
  IN TAS
    GBATASInputWindow.cpp GBATASInputWindow.h GCTASInputWindow.cpp GCTASInputWindow.h
    IRWidget.cpp IRWidget.h StickWidget.cpp StickWidget.h TASCheckBox.cpp TASCheckBox.h
    TASControlState.cpp TASControlState.h TASInputWindow.cpp TASInputWindow.h TASSlider.cpp
    TASSlider.h TASSpinBox.cpp TASSpinBox.h WiiTASInputWindow.cpp WiiTASInputWindow.h
)
set_target_properties(dolphin-emu PROPERTIES
  OUTPUT_NAME PixelU-Wii
  DEBUG_POSTFIX D
  WIN32_EXECUTABLE TRUE
)

target_compile_definitions(dolphin-emu PRIVATE
  QT_USE_QSTRINGBUILDER
  QT_NO_CAST_FROM_ASCII
  QT_NO_CAST_TO_ASCII
  # No deprecation warnings for APIs deprecated after Qt 6.5.1.
  QT_WARN_DEPRECATED_UP_TO=0x060501
)
target_include_directories(dolphin-emu PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  ${Qt6Gui_PRIVATE_INCLUDE_DIRS}
)

target_link_libraries(dolphin-emu PRIVATE
  core
  Qt6::Widgets
  uicommon
  imgui
  implot
)
if(ENABLE_SDL)
  target_link_libraries(dolphin-emu PRIVATE SDL3::SDL3)
endif()
target_link_libraries(dolphin-emu PRIVATE ${qt_gui_private})
if(WIN32)
  target_link_libraries(dolphin-emu PRIVATE
    gdi32.lib
    shell32.lib
    dwmapi.lib  # dark title bars
  )
else()
  # Linux: SIGINT/SIGTERM reach the GUI thread through SignalDaemon, and the render window is
  # docked into RetroPixels through X11 (QtUtils/X11Docking; under Wayland Qt runs on XWayland,
  # see Main.cpp). That goes through Qt's own X connection (libxcb, which Qt's xcb platform
  # uses too; building needs libxcb1-dev, which libx11-dev brings along).
  pixelu_add_sources(dolphin-emu
    IN QtUtils
      SignalDaemon.cpp SignalDaemon.h X11Docking.cpp X11Docking.h
  )
  # Qt D-Bus (part of Qt's base module, which Qt Gui itself uses on Linux): the desktop's
  # screen saver is held off while a game runs (MainWindow), and "Open Containing Folder"
  # selects the file in the file manager (QtUtils).
  find_package(Qt6 QUIET COMPONENTS DBus)
  if(TARGET Qt6::DBus)
    target_link_libraries(dolphin-emu PRIVATE Qt6::DBus)
  else()
    message(STATUS "Qt D-Bus not found: no screen saver inhibition on Linux")
  endif()
  if(ENABLE_X11)
    find_package(X11)
    if(NOT X11_xcb_FOUND)
      message(FATAL_ERROR "Docking into RetroPixels needs libxcb's headers (libxcb1-dev), "
        "or configure with -DENABLE_X11=OFF.")
    endif()
    target_link_libraries(dolphin-emu PRIVATE X11::xcb)
    target_compile_definitions(dolphin-emu PRIVATE PIXELU_QT_X11=1)

    # pixelu-wii-dock-check: plays RetroPixels' part and checks the game window (plain,
    # docked, given up). Xlib only; not built unless asked for (ninja pixelu-wii-dock-check).
    add_executable(pixelu-wii-dock-check EXCLUDE_FROM_ALL)
    pixelu_add_sources(pixelu-wii-dock-check
      IN LinuxCheck
        DockCheck.cpp
    )
    set_target_properties(pixelu-wii-dock-check PROPERTIES AUTOMOC OFF AUTORCC OFF)
    target_link_libraries(pixelu-wii-dock-check PRIVATE X11::X11)
  endif()
endif()

if(MSVC)
  # Qt's headers are external headers: the project's warning level does not apply.
  list(TRANSFORM Qt6Gui_INCLUDE_DIRS PREPEND "/external:I" OUTPUT_VARIABLE qt_external)
  list(TRANSFORM Qt6Gui_PRIVATE_INCLUDE_DIRS PREPEND "/external:I" OUTPUT_VARIABLE qt_external_private)
  target_compile_options(dolphin-emu PRIVATE "${qt_external}" "${qt_external_private}")
  # Qt's headers still use std::aligned_storage.
  target_compile_definitions(dolphin-emu PRIVATE _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING)
  # Qt needs RTTI, which the rest of the project builds without.
  pixelu_drop_option(dolphin-emu /GR-)
  target_compile_options(dolphin-emu PRIVATE /GR)
endif()

if(USE_MGBA)
  pixelu_add_sources(dolphin-emu
    IN .
      GBAHost.cpp GBAHost.h GBAWidget.cpp GBAWidget.h
  )
endif()

if(USE_DISCORD_PRESENCE)
  target_compile_definitions(dolphin-emu PRIVATE USE_DISCORD_PRESENCE)
endif()

if(USE_RETRO_ACHIEVEMENTS)
  target_link_libraries(dolphin-emu PRIVATE rcheevos)
  target_compile_definitions(dolphin-emu PRIVATE USE_RETRO_ACHIEVEMENTS)
  if(RC_CLIENT_SUPPORTS_RAINTEGRATION)
    target_compile_definitions(dolphin-emu PRIVATE RC_CLIENT_SUPPORTS_RAINTEGRATION)
  endif()
endif()

########################################
# The runnable folder (build/Binaries): next to PixelU-Wii.exe go Sys, the
# licence files, qt.conf, the Qt runtime and the translations.

set(bin_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# Sys is PixelU-Wii's own Data/Sys plus a short list of files from the third-party
# checkout's Data/Sys (CMake/PixelUAssembleSys.cmake), assembled fresh on every build.
add_custom_command(TARGET dolphin-emu POST_BUILD
  COMMAND "${CMAKE_COMMAND}"
          "-DTHIRD_PARTY_SYS=${PIXELU_WII_DOLPHIN_DATA}/Data/Sys"
          "-DOWN_SYS=${PROJECT_SOURCE_DIR}/Data/Sys"
          "-DOUT=${bin_dir}/Sys"
          -P "${PIXELU_WII_CMAKE_DIR}/PixelUAssembleSys.cmake"
  COMMENT "Assembling Binaries/Sys"
  VERBATIM)
pixelu_after_build_link(dolphin-emu "${PROJECT_SOURCE_DIR}/COPYING" "${bin_dir}/COPYING")
pixelu_after_build_link(dolphin-emu "${PROJECT_SOURCE_DIR}/LICENSES" "${bin_dir}/Licenses")
if(WIN32)
  pixelu_after_build_link(dolphin-emu "${CMAKE_CURRENT_SOURCE_DIR}/qt.conf.win" "${bin_dir}/qt.conf")
endif()
if(ENABLE_AUTOUPDATE)
  pixelu_after_build_link(dolphin-emu "${CMAKE_BINARY_DIR}/build_info.txt" "${bin_dir}/build_info.txt")
endif()

# On Linux the program in Binaries runs against the Qt it was built with (its
# RUNPATH is $ORIGIN/lib, so a system Qt, or LD_LIBRARY_PATH for the aqt one);
# tools/pixelu-wii-linux-deploy.sh makes the portable folder with Qt's
# libraries in lib/ and its plugins in QtPlugins/.
if(WIN32)
# windeployqt copies the Qt DLLs and plugins PixelU-Wii.exe needs. It runs
# through qtpaths.bat, which gives qtpaths a qt.conf (target_qt.conf) with
# paths relative to the bundled Qt rather than those of the machine Qt was
# built on, and with Qt's bin folder as the only PATH.
get_target_property(moc_exe Qt6::moc IMPORTED_LOCATION)
get_filename_component(QT_BINARY_DIRECTORY "${moc_exe}" DIRECTORY)
find_program(WINDEPLOYQT_EXE windeployqt HINTS "${QT_BINARY_DIRECTORY}" REQUIRED)
find_program(QTPATHS_EXE NAMES qtpaths6 qtpaths HINTS "${QT_BINARY_DIRECTORY}" REQUIRED)
configure_file(target_qt.conf.in "${CMAKE_BINARY_DIR}/target_qt.conf" @ONLY)
configure_file(qtpaths.bat.in "${CMAKE_BINARY_DIR}/qtpaths.bat" @ONLY)
add_custom_command(TARGET dolphin-emu POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${QT_BINARY_DIRECTORY}"
          "${WINDEPLOYQT_EXE}"
            "--qtpaths=${CMAKE_BINARY_DIR}/qtpaths.bat"
            "--libdir=${bin_dir}"
            "--plugindir=${bin_dir}/QtPlugins"
            $<IF:$<CONFIG:Debug>,--debug,--release>
            --no-translations
            --no-compiler-runtime
            --no-system-d3d-compiler
            --no-system-dxc-compiler
            --no-opengl-sw
            # The official Qt build has these image formats and the touch
            # input plugin; PixelU-Wii uses none of them (PNG and SVG only).
            --exclude-plugins qgif,qico,qjpeg,qtuiotouchplugin
            "$<TARGET_FILE:dolphin-emu>"
  VERBATIM)
endif()

# Translations: each Languages/po/<lang>.po is compiled to
# Binaries/Languages/<lang>/dolphin-emu.mo with GNU gettext's msgfmt (found on
# PATH, or given as -DGETTEXT_MSGFMT_EXECUTABLE=...; the workflow installs
# MSYS2's gettext-tools). Without msgfmt there are no translations.
find_package(Gettext)
if(GETTEXT_MSGFMT_EXECUTABLE)
  message(STATUS "msgfmt: ${GETTEXT_MSGFMT_EXECUTABLE}")
endif()
if(GETTEXT_MSGFMT_EXECUTABLE)
  set(po_dir "${PIXELU_WII_DOLPHIN_DATA}/Languages/po")
  set(pot_file "${po_dir}/dolphin-emu.pot")
  file(GLOB po_files "${po_dir}/*.po")
  target_sources(dolphin-emu PRIVATE "${pot_file}" ${po_files})
  source_group("Localization" FILES ${po_files})
  source_group("Localization\\\\Generated" FILES "${pot_file}")
  foreach(po IN LISTS po_files)
    get_filename_component(lang "${po}" NAME_WE)
    set(mo "${bin_dir}/Languages/${lang}/dolphin-emu.mo")
    add_custom_command(OUTPUT "${mo}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${bin_dir}/Languages/${lang}"
      COMMAND "${GETTEXT_MSGFMT_EXECUTABLE}" -o "${mo}" "${po}"
      DEPENDS "${po}"
      VERBATIM)
    target_sources(dolphin-emu PRIVATE "${mo}")
    source_group("Localization\\\\Generated" FILES "${mo}")
  endforeach()
endif()
