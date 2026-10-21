# Core: the emulated GameCube/Wii (CPU, DSP, hardware, IOS, file systems, boot,
# netplay, movies, achievements) and the code that runs it.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(core
  IN .
    AchievementManager.cpp AchievementManager.h ActionReplay.cpp ActionReplay.h ARDecrypt.cpp
    ARDecrypt.h BootManager.cpp BootManager.h CheatCodes.h CheatGeneration.cpp
    CheatGeneration.h CheatSearch.cpp CheatSearch.h CommonTitles.h ConfigManager.cpp
    ConfigManager.h Core.cpp Core.h CoreTiming.cpp CoreTiming.h CPUThreadConfigCallback.cpp
    CPUThreadConfigCallback.h DolphinAnalytics.cpp DolphinAnalytics.h DSPEmulator.cpp
    DSPEmulator.h FreeLookManager.cpp FreeLookManager.h GeckoCode.cpp GeckoCode.h
    GeckoCodeConfig.cpp GeckoCodeConfig.h Host.h HotkeyManager.cpp HotkeyManager.h
    LibusbUtils.cpp LibusbUtils.h MemTools.cpp MemTools.h Movie.cpp Movie.h NetPlayClient.cpp
    NetPlayClient.h NetPlayCommon.cpp NetPlayCommon.h NetPlayServer.cpp NetPlayServer.h
    NetworkCaptureLogger.cpp NetworkCaptureLogger.h PatchEngine.cpp PatchEngine.h State.cpp
    State.h SyncIdentifier.h SysConf.cpp SysConf.h System.cpp System.h TimePlayed.cpp
    TimePlayed.h TitleDatabase.cpp TitleDatabase.h USBUtils.cpp USBUtils.h WC24PatchEngine.cpp
    WC24PatchEngine.h WiiRoot.cpp WiiRoot.h WiiUtils.cpp WiiUtils.h
  IN Boot
    AncastTypes.h Boot.cpp Boot.h Boot_BS2Emu.cpp Boot_WiiWAD.cpp DolReader.cpp DolReader.h
    ElfReader.cpp ElfReader.h ElfTypes.h
  IN Config
    AchievementSettings.cpp AchievementSettings.h DefaultLocale.cpp DefaultLocale.h
    FreeLookSettings.cpp FreeLookSettings.h GraphicsSettings.cpp GraphicsSettings.h
    MainSettings.cpp MainSettings.h NetplaySettings.cpp NetplaySettings.h SessionSettings.cpp
    SessionSettings.h SYSCONFSettings.cpp SYSCONFSettings.h UISettings.cpp UISettings.h
    WiimoteSettings.cpp WiimoteSettings.h
  IN ConfigLoaders
    BaseConfigLoader.cpp BaseConfigLoader.h GameConfigLoader.cpp GameConfigLoader.h
    IsSettingSaveable.cpp IsSettingSaveable.h MovieConfigLoader.cpp MovieConfigLoader.h
    NetPlayConfigLoader.cpp NetPlayConfigLoader.h
  IN DSP
    DSPAccelerator.cpp DSPAccelerator.h DSPAnalyzer.cpp DSPAnalyzer.h DSPAssembler.cpp
    DSPAssembler.h DSPCaptureLogger.cpp DSPCaptureLogger.h DSPCodeUtil.cpp DSPCodeUtil.h
    DSPCommon.h DSPCore.cpp DSPCore.h DSPDisassembler.cpp DSPDisassembler.h DSPHWInterface.cpp
    DSPMemoryMap.cpp DSPStacks.cpp DSPTables.cpp DSPTables.h LabelMap.cpp LabelMap.h
  IN DSP/Interpreter
    DSPIntArithmetic.cpp DSPIntBranch.cpp DSPIntCCUtil.h DSPInterpreter.cpp DSPInterpreter.h
    DSPIntExtOps.cpp DSPIntLoadStore.cpp DSPIntMisc.cpp DSPIntMultiplier.cpp DSPIntTables.cpp
    DSPIntTables.h DSPIntUtil.h
  IN DSP/Jit
    DSPEmitterBase.cpp DSPEmitterBase.h
  IN DSP/Jit/x64
    DSPEmitter.cpp DSPEmitter.h DSPJitArithmetic.cpp DSPJitBranch.cpp DSPJitCCUtil.cpp
    DSPJitExtOps.cpp DSPJitLoadStore.cpp DSPJitMisc.cpp DSPJitMultiplier.cpp DSPJitRegCache.cpp
    DSPJitRegCache.h DSPJitTables.cpp DSPJitTables.h DSPJitUtil.cpp
  IN Debugger
    BranchWatch.cpp BranchWatch.h CodeTrace.cpp CodeTrace.h Debugger_SymbolMap.cpp
    Debugger_SymbolMap.h DebugInterface.h Dump.cpp Dump.h OSThread.cpp OSThread.h
    PPCDebugInterface.cpp PPCDebugInterface.h RSO.cpp RSO.h
  IN FifoPlayer
    FifoDataFile.cpp FifoDataFile.h FifoPlayer.cpp FifoPlayer.h FifoRecorder.cpp FifoRecorder.h
  IN HLE
    HLE.cpp HLE.h HLE_Misc.cpp HLE_Misc.h HLE_OS.cpp HLE_OS.h HLE_VarArgs.cpp HLE_VarArgs.h
  IN HW
    AddressSpace.cpp AddressSpace.h AudioInterface.cpp AudioInterface.h CPU.cpp CPU.h DSP.cpp
    DSP.h GBAPad.cpp GBAPad.h GBAPadEmu.cpp GBAPadEmu.h GCKeyboard.cpp GCKeyboard.h
    GCKeyboardEmu.cpp GCKeyboardEmu.h GCPad.cpp GCPad.h GCPadEmu.cpp GCPadEmu.h GPFifo.cpp
    GPFifo.h HW.cpp HW.h Memmap.cpp Memmap.h MemoryInterface.cpp MemoryInterface.h MMIO.cpp
    MMIO.h ProcessorInterface.cpp ProcessorInterface.h Sram.cpp Sram.h StreamADPCM.cpp
    StreamADPCM.h SystemTimers.cpp SystemTimers.h VideoInterface.cpp VideoInterface.h
    WII_IPC.cpp WII_IPC.h Wiimote.cpp Wiimote.h WiiSave.cpp WiiSave.h WiiSaveStructs.h
  IN HW/DSPHLE
    DSPHLE.cpp DSPHLE.h MailHandler.cpp MailHandler.h
  IN HW/DSPHLE/UCodes
    AESnd.cpp AESnd.h ASnd.cpp ASnd.h AX.cpp AX.h AXStructs.h AXVoice.h AXWii.cpp AXWii.h
    CARD.cpp CARD.h GBA.cpp GBA.h INIT.cpp INIT.h ROM.cpp ROM.h UCodes.cpp UCodes.h Zelda.cpp
    Zelda.h
  IN HW/DSPLLE
    DSPHost.cpp DSPLLE.cpp DSPLLE.h DSPSymbols.cpp DSPSymbols.h
  IN HW/DVD
    AMMediaboard.cpp AMMediaboard.h DVDInterface.cpp DVDInterface.h DVDMath.cpp DVDMath.h
    DVDThread.cpp DVDThread.h FileMonitor.cpp FileMonitor.h
  IN HW/EXI
    EXI.cpp EXI.h EXI_Channel.cpp EXI_Channel.h EXI_Device.cpp EXI_Device.h EXI_DeviceAD16.cpp
    EXI_DeviceAD16.h EXI_DeviceAGP.cpp EXI_DeviceAGP.h EXI_DeviceBaseboard.cpp
    EXI_DeviceBaseboard.h EXI_DeviceDummy.cpp EXI_DeviceDummy.h EXI_DeviceEthernet.cpp
    EXI_DeviceEthernet.h EXI_DeviceGecko.cpp EXI_DeviceGecko.h EXI_DeviceIPL.cpp
    EXI_DeviceIPL.h EXI_DeviceMemoryCard.cpp EXI_DeviceMemoryCard.h EXI_DeviceModem.cpp
    EXI_DeviceModem.h
  IN HW/EXI/BBA
    BuiltIn.cpp BuiltIn.h TAP_Win32.cpp TAP_Win32.h TAPServerBBA.cpp TAPServerConnection.cpp
    XLINK_KAI_BBA.cpp
  IN HW/EXI/Modem
    TAPServerModem.cpp
  IN HW/GCMemcard
    GCIFile.cpp GCIFile.h GCMemcard.cpp GCMemcard.h GCMemcardBase.h GCMemcardDirectory.cpp
    GCMemcardDirectory.h GCMemcardRaw.cpp GCMemcardRaw.h GCMemcardUtils.cpp GCMemcardUtils.h
  IN HW/HSP
    HSP.cpp HSP.h HSP_Device.cpp HSP_Device.h HSP_DeviceARAMExpansion.cpp
    HSP_DeviceARAMExpansion.h HSP_DeviceGBPlayer.cpp HSP_DeviceGBPlayer.h HSP_DeviceNull.cpp
    HSP_DeviceNull.h
  IN HW/MagCard
    C1231BR.cpp C1231BR.h C1231LR.cpp C1231LR.h MagneticCardReader.cpp MagneticCardReader.h
  IN HW/SI
    SI.cpp SI.h SI_Device.cpp SI_Device.h SI_DeviceAMBaseboard.cpp SI_DeviceAMBaseboard.h
    SI_DeviceDanceMat.cpp SI_DeviceDanceMat.h SI_DeviceGBA.cpp SI_DeviceGBA.h
    SI_DeviceGCAdapter.cpp SI_DeviceGCAdapter.h SI_DeviceGCController.cpp
    SI_DeviceGCController.h SI_DeviceGCSteeringWheel.cpp SI_DeviceGCSteeringWheel.h
    SI_DeviceKeyboard.cpp SI_DeviceKeyboard.h SI_DeviceNull.cpp SI_DeviceNull.h
  IN HW/Triforce
    DeckReader.cpp DeckReader.h FZeroAX.cpp FZeroAX.h ICCardReader.cpp ICCardReader.h
    IOPorts.cpp IOPorts.h JVSIO.cpp JVSIO.h MarioKartGP.cpp MarioKartGP.h SerialDevice.cpp
    SerialDevice.h Touchscreen.cpp Touchscreen.h
  IN HW/WiimoteCommon
    DataReport.cpp DataReport.h WiimoteConstants.h WiimoteHid.h WiimoteReport.h
  IN HW/WiimoteEmu
    Camera.cpp Camera.h DesiredWiimoteState.cpp DesiredWiimoteState.h Dynamics.cpp Dynamics.h
    EmuSubroutines.cpp Encryption.cpp Encryption.h ExtensionPort.cpp ExtensionPort.h I2CBus.cpp
    I2CBus.h MotionPlus.cpp MotionPlus.h Speaker.cpp Speaker.h WiimoteEmu.cpp WiimoteEmu.h
  IN HW/WiimoteEmu/Extension
    Classic.cpp Classic.h DesiredExtensionState.h DrawsomeTablet.cpp DrawsomeTablet.h Drums.cpp
    Drums.h Extension.cpp Extension.h Guitar.cpp Guitar.h Nunchuk.cpp Nunchuk.h Shinkansen.cpp
    Shinkansen.h TaTaCon.cpp TaTaCon.h Turntable.cpp Turntable.h UDrawTablet.cpp UDrawTablet.h
  IN HW/WiimoteReal
    IOWin.cpp IOWin.h WiimoteReal.cpp WiimoteReal.h
  IN IOS
    Device.cpp Device.h DeviceStub.cpp DeviceStub.h DolphinDevice.cpp DolphinDevice.h IOS.cpp
    IOS.h IOSC.cpp IOSC.h MIOS.cpp MIOS.h VersionInfo.cpp VersionInfo.h
  IN IOS/Crypto
    AesDevice.cpp AesDevice.h Sha.cpp Sha.h
  IN IOS/DI
    DI.cpp DI.h
  IN IOS/ES
    ES.cpp ES.h Formats.cpp Formats.h Identity.cpp NandUtils.cpp TitleContents.cpp
    TitleInformation.cpp TitleManagement.cpp Views.cpp
  IN IOS/FS
    FileSystem.h FileSystemCommon.cpp FileSystemProxy.cpp FileSystemProxy.h
  IN IOS/FS/HostBackend
    File.cpp FS.cpp FS.h
  IN IOS/Network
    ICMP.h ICMPLin.cpp MACUtils.cpp MACUtils.h Socket.cpp Socket.h SSL.cpp SSL.h
  IN IOS/Network/IP
    Top.cpp Top.h
  IN IOS/Network/KD
    NetKDRequest.cpp NetKDRequest.h NetKDTime.cpp NetKDTime.h NWC24Config.cpp NWC24Config.h
    NWC24DL.cpp NWC24DL.h WC24File.h
  IN IOS/Network/KD/Mail
    MailCommon.h WC24FriendList.cpp WC24FriendList.h WC24Send.cpp WC24Send.h
  IN IOS/Network/KD/VFF
    VFFUtil.cpp VFFUtil.h
  IN IOS/Network/NCD
    Manage.cpp Manage.h WiiNetConfig.cpp WiiNetConfig.h
  IN IOS/Network/WD
    Command.cpp Command.h
  IN IOS/SDIO
    SDIOSlot0.cpp SDIOSlot0.h
  IN IOS/STM
    STM.cpp STM.h
  IN IOS/USB
    Common.cpp Common.h Host.cpp Host.h USB_KBD.cpp USB_KBD.h USBScanner.cpp USBScanner.h
    USBV0.cpp USBV0.h USBV4.cpp USBV4.h USBV5.cpp USBV5.h
  IN IOS/USB/Bluetooth
    BTBase.cpp BTBase.h BTEmu.cpp BTEmu.h BTStub.cpp BTStub.h hci.h l2cap.h WiimoteDevice.cpp
    WiimoteDevice.h WiimoteHIDAttr.cpp WiimoteHIDAttr.h
  IN IOS/USB/Emulated
    Infinity.cpp Infinity.h LogitechMic.cpp LogitechMic.h Microphone.cpp Microphone.h
    WiiSpeak.cpp WiiSpeak.h
  IN IOS/USB/Emulated/Skylanders
    Skylander.cpp Skylander.h SkylanderCrypto.cpp SkylanderCrypto.h SkylanderFigure.cpp
    SkylanderFigure.h
  IN IOS/USB/OH0
    OH0.cpp OH0.h OH0Device.cpp OH0Device.h
  IN IOS/USB/USB_HID
    HIDv4.cpp HIDv4.h HIDv5.cpp HIDv5.h
  IN IOS/USB/USB_VEN
    VEN.cpp VEN.h
  IN IOS/WFS
    WFSI.cpp WFSI.h WFSSRV.cpp WFSSRV.h
  IN PowerPC
    BreakPoints.cpp BreakPoints.h ConditionRegister.cpp ConditionRegister.h Expression.cpp
    Expression.h GDBStub.cpp GDBStub.h JitInterface.cpp JitInterface.h MMU.cpp MMU.h
    PowerPC.cpp PowerPC.h PPCAnalyst.cpp PPCAnalyst.h PPCCache.cpp PPCCache.h PPCSymbolDB.cpp
    PPCSymbolDB.h PPCTables.cpp PPCTables.h
  IN PowerPC/CachedInterpreter
    CachedInterpreter.cpp CachedInterpreter.h CachedInterpreter_Disassembler.cpp
    CachedInterpreterBlockCache.cpp CachedInterpreterBlockCache.h CachedInterpreterEmitter.cpp
    CachedInterpreterEmitter.h
  IN PowerPC/Interpreter
    ExceptionUtils.h Interpreter.cpp Interpreter.h Interpreter_Branch.cpp
    Interpreter_FloatingPoint.cpp Interpreter_FPUtils.h Interpreter_Integer.cpp
    Interpreter_LoadStore.cpp Interpreter_LoadStorePaired.cpp Interpreter_Paired.cpp
    Interpreter_SystemRegisters.cpp Interpreter_Tables.cpp
  IN PowerPC/Jit64
    Jit.cpp Jit.h Jit64_Tables.cpp Jit_Branch.cpp Jit_FloatingPoint.cpp Jit_Integer.cpp
    Jit_LoadStore.cpp Jit_LoadStoreFloating.cpp Jit_LoadStorePaired.cpp Jit_Paired.cpp
    Jit_SystemRegisters.cpp JitAsm.cpp JitAsm.h
  IN PowerPC/Jit64/RegCache
    CachedReg.h FPURegCache.cpp FPURegCache.h GPRRegCache.cpp GPRRegCache.h JitRegCache.cpp
    JitRegCache.h RCMode.h
  IN PowerPC/Jit64Common
    BlockCache.cpp BlockCache.h ConstantPool.cpp ConstantPool.h EmuCodeBlock.cpp EmuCodeBlock.h
    FarCodeCache.cpp FarCodeCache.h Jit64AsmCommon.cpp Jit64AsmCommon.h Jit64Constants.h
    Jit64PowerPCState.h TrampolineCache.cpp TrampolineCache.h TrampolineInfo.h
  IN PowerPC/JitCommon
    ConstantPropagation.cpp ConstantPropagation.h DivUtils.cpp DivUtils.h JitAsmCommon.cpp
    JitAsmCommon.h JitBase.cpp JitBase.h JitCache.cpp JitCache.h
  IN PowerPC/SignatureDB
    CSVSignatureDB.cpp CSVSignatureDB.h DSYSignatureDB.cpp DSYSignatureDB.h MEGASignatureDB.cpp
    MEGASignatureDB.h SignatureDB.cpp SignatureDB.h
)

# The hash of the approved per-game INI list used by RetroAchievements hardcore
# mode, taken from the Sys/ApprovedInis.json that the build ships.
if(EXISTS "${PROJECT_SOURCE_DIR}/Data/Sys/ApprovedInis.json")
  set(approved_inis "${PROJECT_SOURCE_DIR}/Data/Sys/ApprovedInis.json")
else()
  set(approved_inis "${PIXELU_WII_DOLPHIN_DATA}/Data/Sys/ApprovedInis.json")
endif()
set(approved_hash_h "${CMAKE_CURRENT_BINARY_DIR}/AchievementApprovedHash.h")
add_custom_command(
  OUTPUT "${approved_hash_h}"
  COMMAND "${CMAKE_COMMAND}"
          "-DJSON_FILE=${approved_inis}"
          "-DTEMPLATE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/AchievementApprovedHash.h.in"
          "-DOUTPUT_FILE=${approved_hash_h}"
          -P "${PIXELU_WII_CMAKE_DIR}/PixelUAchievementHash.cmake"
  DEPENDS "${approved_inis}" "${CMAKE_CURRENT_SOURCE_DIR}/AchievementApprovedHash.h.in"
  COMMENT "Hashing ApprovedInis.json into AchievementApprovedHash.h"
  VERBATIM
)
target_sources(core PRIVATE "${approved_hash_h}")

target_link_libraries(core
  PUBLIC
    audiocommon
    common
    discio
    enet::enet
    expr
    inputcommon
    MbedTLS::mbedtls
    pugixml
    sfml-network
    videonull
    videoogl
    videosoftware
  PRIVATE
    FatFs
    fmt::fmt
    LZO::LZO
    LZ4::LZ4
    ZLIB::ZLIB
)
target_compile_definitions(core PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)

# Optional parts, each with the library it needs.
if(ENABLE_CUBEB)
  # The GameCube microphone.
  target_link_libraries(core PUBLIC cubeb)
  pixelu_add_sources(core
    IN HW/EXI
      EXI_DeviceMic.cpp EXI_DeviceMic.h
  )
endif()

if(TARGET LibUSB::LibUSB)
  # Real USB devices and Bluetooth adapters passed through to the Wii.
  target_link_libraries(core PUBLIC LibUSB::LibUSB)
  pixelu_add_sources(core
    IN IOS/USB
      LibusbDevice.cpp LibusbDevice.h
    IN IOS/USB/Bluetooth
      BTReal.cpp BTReal.h LibUSBBluetoothAdapter.cpp LibUSBBluetoothAdapter.h
      RealtekFirmwareLoader.cpp RealtekFirmwareLoader.h
  )
endif()

if(ENABLE_VULKAN)
  target_link_libraries(core PUBLIC videovulkan)
endif()

if(USE_MGBA)
  # Game Boy Advance link cables, emulated with mGBA.
  pixelu_add_sources(core
    IN HW
      GBACore.cpp GBACore.h
    IN HW/SI
      SI_DeviceGBAEmu.cpp SI_DeviceGBAEmu.h
  )
  target_link_libraries(core PUBLIC mGBA::mgba)
  target_compile_definitions(core PUBLIC HAS_LIBMGBA)
endif()

target_link_libraries(core PUBLIC
  videod3d
  videod3d12
  setupapi.lib
  iphlpapi.lib
)

if(TARGET Hidapi::Hidapi)
  # Real Wii Remotes through hidapi.
  pixelu_add_sources(core
    IN HW/WiimoteReal
      IOhidapi.cpp IOhidapi.h
  )
  target_link_libraries(core PUBLIC Hidapi::Hidapi)
  target_compile_definitions(core PRIVATE HAVE_HIDAPI=1)
endif()

if(TARGET cpp-ipc::ipc)
  # Broadband adapter linked to another program over shared memory.
  pixelu_add_sources(core
    IN HW/EXI/BBA
      IPC.cpp
  )
  target_link_libraries(core PRIVATE cpp-ipc::ipc)
endif()

pixelu_use_pch(core)

if(USE_RETRO_ACHIEVEMENTS)
  target_link_libraries(core PUBLIC rcheevos)
  target_compile_definitions(core PUBLIC USE_RETRO_ACHIEVEMENTS RC_CLIENT_SUPPORTS_HASH)
  if(RC_CLIENT_SUPPORTS_RAINTEGRATION)
    target_compile_definitions(core PUBLIC RC_CLIENT_SUPPORTS_RAINTEGRATION)
  endif()
endif()
