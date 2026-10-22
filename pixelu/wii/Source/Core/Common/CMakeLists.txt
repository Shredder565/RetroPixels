# Common: PixelU-Wii's base library (files, strings, config, logging, crypto, threads,
# the x86-64 emitter, OpenGL context creation, networking helpers).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(common
  IN .
    Analytics.cpp Analytics.h Assert.h BitField.h BitSet.h BitUtils.h BlockingLoop.h Buffer.h
    ChunkFile.h CodeBlock.h ColorUtil.cpp ColorUtil.h Common.h CommonFuncs.cpp CommonFuncs.h
    CommonPaths.h CommonTypes.h Contains.h CPUDetect.h CWDemangler.cpp
    CWDemangler.h DirectIOFile.cpp DirectIOFile.h DynamicLibrary.cpp DynamicLibrary.h ENet.cpp
    ENet.h EnumFormatter.h EnumMap.h Event.h FatFsUtil.cpp FatFsUtil.h FileSearch.cpp
    FileSearch.h FilesystemWatcher.cpp FilesystemWatcher.h FileUtil.cpp FileUtil.h
    FixedSizeQueue.h Flag.h FloatUtils.cpp FloatUtils.h FormatUtil.h FPURoundMode.h
    Functional.h GekkoDisassembler.cpp GekkoDisassembler.h Hash.cpp Hash.h HookableEvent.h
    HostDisassembler.cpp HostDisassembler.h HttpRequest.cpp HttpRequest.h
    Image.cpp Image.h IniFile.cpp IniFile.h Inline.h IOFile.cpp IOFile.h JitRegister.cpp
    JitRegister.h JsonUtil.cpp JsonUtil.h Lazy.h LinearDiskCache.h
    MathUtil.h Matrix.cpp Matrix.h MemArena.h MemoryUtil.cpp MemoryUtil.h
    MinizipUtil.h MsgHandler.cpp MsgHandler.h Mutex.h NandPaths.cpp NandPaths.h Network.cpp
    Network.h OneShotEvent.h PcapFile.cpp PcapFile.h Profiler.cpp Profiler.h Projection.h
    QoSSession.cpp QoSSession.h Random.cpp Random.h RangeSet.h RangeSizeSet.h ScopeGuard.h
    SDCardUtil.cpp SDCardUtil.h SettingsHandler.cpp SettingsHandler.h SFMLHelper.cpp
    SFMLHelper.h SmallVector.h SocketContext.cpp SocketContext.h SpanUtils.h SPSCQueue.h
    StringUtil.cpp StringUtil.h SymbolDB.cpp SymbolDB.h Thread.cpp Thread.h Timer.cpp Timer.h
    TimeUtil.cpp TimeUtil.h TransferableSharedMutex.h TraversalClient.cpp TraversalClient.h
    TraversalProto.h TypeUtils.h UPnP.cpp UPnP.h VariantUtil.h Version.cpp Version.h
    WaitableFlag.h WindowSystemInfo.h WorkQueueThread.h x64ABI.cpp x64ABI.h x64CPUDetect.cpp x64Emitter.cpp x64Emitter.h
    x64FPURoundMode.cpp x64Reg.h
  IN Assembler
    AssemblerShared.cpp AssemblerShared.h AssemblerTables.cpp AssemblerTables.h
    GekkoAssembler.cpp GekkoAssembler.h GekkoIRGen.cpp GekkoIRGen.h GekkoLexer.cpp GekkoLexer.h
    GekkoParser.cpp GekkoParser.h
  IN Config
    Config.cpp Config.h ConfigInfo.cpp ConfigInfo.h Enums.h Layer.cpp Layer.h
  IN Crypto
    AES.cpp AES.h bn.cpp bn.h ec.cpp ec.h HMAC.cpp HMAC.h SHA1.cpp SHA1.h
  IN Debug
    MemoryPatches.cpp MemoryPatches.h Threads.h Watches.cpp Watches.h
  IN GL
    GLContext.cpp GLContext.h GLUtil.cpp GLUtil.h
  IN GL/GLExtensions
    GLExtensions.cpp GLExtensions.h
  IN Logging
    ConsoleListener.h Log.h LogManager.cpp LogManager.h
)

if(WIN32)
  pixelu_add_sources(common
    IN .
      CompatPatches.cpp HRWrap.cpp HRWrap.h LdrWatcher.cpp LdrWatcher.h MemArenaWin.cpp
      WindowsDevice.cpp WindowsDevice.h WindowsRegistry.cpp
    IN GL/GLInterface
      WGL.cpp WGL.h
    IN Logging
      ConsoleListenerWin.cpp
  )
else()
  # Linux: shared memory through memfd/mmap, log lines on standard error, and the GLX and EGL
  # OpenGL contexts.
  pixelu_add_sources(common
    IN .
      MemArenaUnix.cpp
    IN GL/GLInterface
      EGL.cpp EGL.h GLX.cpp GLX.h X11Window.cpp X11Window.h
    IN Logging
      ConsoleListenerNix.cpp
  )

  # libX11, libGL (GLX), libEGL and libwayland-egl are opened at run time, so nothing is
  # linked for them; only the Xlib and EGL headers are needed to build.
  find_path(PIXELU_X11_INCLUDE_DIR X11/Xlib.h)
  find_path(PIXELU_EGL_INCLUDE_DIR EGL/egl.h)
  if(NOT PIXELU_X11_INCLUDE_DIR OR NOT PIXELU_EGL_INCLUDE_DIR)
    message(FATAL_ERROR "The X11 and EGL headers are missing (Debian/Ubuntu: libx11-dev libegl-dev).")
  endif()
  target_include_directories(common PRIVATE ${PIXELU_X11_INCLUDE_DIR} ${PIXELU_EGL_INCLUDE_DIR})
  target_compile_definitions(common PRIVATE PIXELU_GL_GLX_EGL)
endif()

# PixelU-Wii's own version header (Common/scmrev.h), rewritten on every build.
include(${CMAKE_CURRENT_SOURCE_DIR}/PixelUWiiVersion.cmake)
add_dependencies(common pixelu_wii_version)

target_link_libraries(common
  PUBLIC
    ${CMAKE_THREAD_LIBS_INIT}
    enet::enet
    fmt::fmt
    MbedTLS::mbedtls
    MINIZIP::minizip-ng
    sfml-network
  PRIVATE
    CURL::libcurl
    FatFs
    spng::spng
    watcher
    bdisasm
)

if(WIN32)
  target_link_libraries(common PRIVATE kernel32.lib shlwapi.lib winmm.lib opengl32.lib)
else()
  # dlopen (DynamicLibrary) and shm_open (MemArenaUnix.cpp) are in libc itself since
  # glibc 2.34; libdl and librt are named anyway so older C libraries link too.
  target_link_libraries(common PRIVATE ${CMAKE_DL_LIBS} rt)
endif()

if(USE_UPNP)
  target_link_libraries(common PRIVATE Miniupnpc::miniupnpc)
endif()

# Host-code disassembly in the debugger uses LLVM when its CMake package is
# installed; otherwise Bochs' disassembler (bdisasm, above) does it.
if(ENABLE_LLVM)
  find_package(LLVM CONFIG)
  if(LLVM_FOUND)
    message(STATUS "LLVM found: the debugger disassembles host code with it")
    target_compile_definitions(common PRIVATE HAVE_LLVM)
    llvm_expand_pseudo_components(llvm_targets AllTargetsInfos AllTargetsDisassemblers AllTargetsCodeGens)
    llvm_config(common USE_SHARED mcdisassembler target ${llvm_targets})
    target_include_directories(common PRIVATE ${LLVM_INCLUDE_DIRS})
  endif()
endif()

if(WIN32)
  # build_info.txt records the Visual C++ runtime this build needs (for an updater),
  # read from the vc_redist.x64.exe of the Visual Studio that builds it.
  find_program(POWERSHELL_EXE NAMES powershell REQUIRED)
  execute_process(
    COMMAND "${POWERSHELL_EXE}" -Command
      "[System.Diagnostics.FileVersionInfo]::GetVersionInfo('$ENV{VCToolsRedistDir}vc_redist.x64.exe').ProductVersion"
    OUTPUT_VARIABLE VC_TOOLS_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/build_info.txt.in" "${CMAKE_BINARY_DIR}/build_info.txt")

  # CompatPatches.cpp patches system DLLs at load time; keep it although nothing
  # references it.
  target_link_libraries(common PRIVATE "-INCLUDE:enableCompatPatches")
endif()

pixelu_use_pch(common)
