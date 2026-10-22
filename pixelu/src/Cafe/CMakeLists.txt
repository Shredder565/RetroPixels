# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuCafe: the emulated console (CPU, GPU, IOSU, OS libraries, file systems)

add_library(CemuCafe)
pixelu_add_folder(CemuCafe .
	CafeSystem.cpp
	CafeSystem.h
	GamePatch.cpp
	GamePatch.h
)
pixelu_add_folder(CemuCafe Account Account.cpp Account.h AccountError.h)
pixelu_add_folder(CemuCafe Filesystem
	fsc.cpp
	fsc.h
	fscDeviceHostFS.cpp
	fscDeviceHostFS.h
	fscDeviceRedirect.cpp
	fscDeviceWua.cpp
	fscDeviceWud.cpp
	fscDeviceWuhb.cpp
)
pixelu_add_folder(CemuCafe Filesystem/FST
	FST.cpp
	FST.h
	fstUtil.h
	KeyCache.cpp
	KeyCache.h
)
pixelu_add_folder(CemuCafe Filesystem/WUD wud.cpp wud.h)
pixelu_add_folder(CemuCafe Filesystem/WUHB RomFSStructs.h WUHBReader.cpp WUHBReader.h)
pixelu_add_folder(CemuCafe GameProfile GameProfile.cpp GameProfile.h)
pixelu_add_folder(CemuCafe GraphicPack
	GraphicPack2.cpp
	GraphicPack2.h
	GraphicPack2Patches.cpp
	GraphicPack2Patches.h
	GraphicPack2PatchesApply.cpp
	GraphicPack2PatchesParser.cpp
	GraphicPackError.h
)
pixelu_add_folder(CemuCafe HW/ACR ACR.cpp)
pixelu_add_folder(CemuCafe HW/AI AI.cpp AI.h)
pixelu_add_folder(CemuCafe HW/Common HwReg.h)
pixelu_add_folder(CemuCafe HW/Espresso
	Const.h
	EspressoISA.h
	PPCCallback.h
	PPCScheduler.cpp
	PPCSchedulerLLE.cpp
	PPCState.h
	PPCTimer.cpp
)
pixelu_add_folder(CemuCafe HW/Espresso/Debugger
	Debugger.cpp
	Debugger.h
	DebugSymbolStorage.cpp
	DebugSymbolStorage.h
	GDBBreakpoints.cpp
	GDBBreakpoints.h
	GDBStub.cpp
	GDBStub.h
)
pixelu_add_folder(CemuCafe HW/Espresso/Interpreter
	PPCInterpreterALU.hpp
	PPCInterpreterFPU.cpp
	PPCInterpreterHelper.h
	PPCInterpreterHLE.cpp
	PPCInterpreterImpl.cpp
	PPCInterpreterInternal.h
	PPCInterpreterLoadStore.hpp
	PPCInterpreterMain.cpp
	PPCInterpreterOPC.cpp
	PPCInterpreterOPC.hpp
	PPCInterpreterPS.cpp
	PPCInterpreterSPR.hpp
)
pixelu_add_folder(CemuCafe HW/Espresso/Recompiler
	PPCFunctionBoundaryTracker.h
	PPCRecompiler.cpp
	PPCRecompiler.h
	PPCRecompilerIml.h
	PPCRecompilerImlGen.cpp
	PPCRecompilerImlGenFPU.cpp
	PPCRecompilerIntermediate.cpp
)
pixelu_add_folder(CemuCafe HW/Espresso/Recompiler/BackendX64
	BackendX64.cpp
	BackendX64.h
	BackendX64AVX.cpp
	BackendX64BMI.cpp
	BackendX64FPU.cpp
	BackendX64Gen.cpp
	BackendX64GenFPU.cpp
	X64Emit.hpp
	x86Emitter.h
)
pixelu_add_folder(CemuCafe HW/Espresso/Recompiler/IML
	IML.h
	IMLAnalyzer.cpp
	IMLDebug.cpp
	IMLInstruction.cpp
	IMLInstruction.h
	IMLOptimizer.cpp
	IMLRegisterAllocator.cpp
	IMLRegisterAllocator.h
	IMLRegisterAllocatorRanges.cpp
	IMLRegisterAllocatorRanges.h
	IMLSegment.cpp
	IMLSegment.h
)
pixelu_add_folder(CemuCafe HW/Latte/Common
	RegisterSerializer.cpp
	RegisterSerializer.h
	ShaderSerializer.cpp
	ShaderSerializer.h
)
pixelu_add_folder(CemuCafe HW/Latte/Core
	FetchShader.cpp
	FetchShader.h
	Latte.h
	LatteAsyncCommands.cpp
	LatteAsyncCommands.h
	LatteBufferCache.cpp
	LatteBufferCache.h
	LatteBufferData.cpp
	LatteCachedFBO.h
	LatteCommandProcessor.cpp
	LatteConst.h
	LatteDraw.h
	LatteGSCopyShaderParser.cpp
	LatteIndices.cpp
	LatteIndices.h
	LatteOverlay.cpp
	LatteOverlay.h
	LattePerformanceMonitor.cpp
	LattePerformanceMonitor.h
	LattePM4.h
	LatteQuery.cpp
	LatteQueryObject.h
	LatteRecorder.cpp
	LatteRecorder.h
	LatteRenderTarget.cpp
	LatteRingBuffer.cpp
	LatteRingBuffer.h
	LatteShader.cpp
	LatteShader.h
	LatteShaderAssembly.h
	LatteShaderCache.cpp
	LatteShaderCache.h
	LatteSoftware.cpp
	LatteSoftware.h
	LatteStreamoutGPU.cpp
	LatteSurfaceCopy.cpp
	LatteSurfaceCopy.h
	LatteTexture.cpp
	LatteTexture.h
	LatteTextureCache.cpp
	LatteTextureLegacy.cpp
	LatteTextureLoader.cpp
	LatteTextureLoader.h
	LatteTextureReadback.cpp
	LatteTextureReadbackInfo.h
	LatteTextureView.cpp
	LatteTextureView.h
	LatteThread.cpp
	LatteTiming.cpp
	LatteTiming.h
)
pixelu_add_folder(CemuCafe HW/Latte/ISA LatteInstructions.h LatteReg.h RegDefines.h)
pixelu_add_folder(CemuCafe HW/Latte/LatteAddrLib
	AddrLibFastDecode.h
	LatteAddrLib.cpp
	LatteAddrLib.h
	LatteAddrLib_Coord.cpp
)
pixelu_add_folder(CemuCafe HW/Latte/LegacyShaderDecompiler
	LatteDecompiler.cpp
	LatteDecompiler.h
	LatteDecompilerAnalyzer.cpp
	LatteDecompilerEmitGLSL.cpp
	LatteDecompilerEmitGLSLAttrDecoder.cpp
	LatteDecompilerEmitGLSLHeader.hpp
	LatteDecompilerInstructions.h
	LatteDecompilerInternal.h
	LatteDecompilerRegisterDataTypeTracker.cpp
)
pixelu_add_folder(CemuCafe HW/Latte/Renderer
	Renderer.cpp
	Renderer.h
	RendererCore.cpp
	RendererCore.h
	RendererOuputShader.cpp
	RendererOuputShader.h
	RendererShader.cpp
	RendererShader.h
)
pixelu_add_folder(CemuCafe HW/Latte/Renderer/OpenGL OpenGLSurfaceCopy.h)
pixelu_add_folder(CemuCafe HW/Latte/ShaderInfo
	ShaderDescription.cpp
	ShaderInfo.h
	ShaderInstanceInfo.cpp
)
pixelu_add_folder(CemuCafe HW/Latte/Transcompiler LatteTC.cpp LatteTC.h LatteTCGenIR.cpp)
pixelu_add_folder(CemuCafe HW/MMU MMU.cpp MMU.h)
pixelu_add_folder(CemuCafe HW/SI SI.cpp si.h)
pixelu_add_folder(CemuCafe HW/VI VI.cpp)
pixelu_add_folder(CemuCafe IOSU iosu_ipc_common.h iosu_types_common.h)
pixelu_add_folder(CemuCafe IOSU/ccr_nfc iosu_ccr_nfc.cpp iosu_ccr_nfc.h)
pixelu_add_folder(CemuCafe IOSU/fsa fsa_types.h iosu_fsa.cpp iosu_fsa.h)
pixelu_add_folder(CemuCafe IOSU/kernel iosu_kernel.cpp iosu_kernel.h)
pixelu_add_folder(CemuCafe IOSU/legacy
	iosu_acp.cpp
	iosu_acp.h
	iosu_act.cpp
	iosu_act.h
	iosu_crypto.cpp
	iosu_crypto.h
	iosu_fpd.cpp
	iosu_fpd.h
	iosu_ioctl.cpp
	iosu_ioctl.h
	iosu_mcp.cpp
	iosu_mcp.h
	iosu_nim.cpp
	iosu_nim.h
)
pixelu_add_folder(CemuCafe IOSU/nn iosu_nn_service.cpp iosu_nn_service.h)
pixelu_add_folder(CemuCafe IOSU/nn/boss
	boss_common.cpp
	boss_common.h
	boss_service.cpp
	boss_service.h
)
pixelu_add_folder(CemuCafe IOSU/ODM iosu_odm.cpp iosu_odm.h)
pixelu_add_folder(CemuCafe IOSU/PDM iosu_pdm.cpp iosu_pdm.h)
pixelu_add_folder(CemuCafe OS/common
	OSCommon.cpp
	OSCommon.h
	OSUtil.h
	PPCConcurrentQueue.h
)
pixelu_add_folder(CemuCafe OS/libs nn_client_service.h nn_common.h)
pixelu_add_folder(CemuCafe OS/libs/avm avm.cpp avm.h)
pixelu_add_folder(CemuCafe OS/libs/camera camera.cpp camera.h)
pixelu_add_folder(CemuCafe OS/libs/coreinit
	coreinit.cpp
	coreinit.h
	coreinit_Alarm.cpp
	coreinit_Alarm.h
	coreinit_Atomic.cpp
	coreinit_Atomic.h
	coreinit_BSP.cpp
	coreinit_BSP.h
	coreinit_Callbacks.cpp
	coreinit_CodeGen.cpp
	coreinit_CodeGen.h
	coreinit_Coroutine.cpp
	coreinit_Coroutine.h
	coreinit_DynLoad.cpp
	coreinit_DynLoad.h
	coreinit_FG.cpp
	coreinit_FG.h
	coreinit_FS.cpp
	coreinit_FS.h
	coreinit_GHS.cpp
	coreinit_GHS.h
	coreinit_HWInterface.cpp
	coreinit_HWInterface.h
	coreinit_IM.cpp
	coreinit_IM.h
	coreinit_Init.cpp
	coreinit_IOS.cpp
	coreinit_IOS.h
	coreinit_IPC.cpp
	coreinit_IPC.h
	coreinit_IPCBuf.cpp
	coreinit_IPCBuf.h
	coreinit_LockedCache.cpp
	coreinit_LockedCache.h
	coreinit_MCP.cpp
	coreinit_MCP.h
	coreinit_MEM.cpp
	coreinit_MEM.h
	coreinit_MEM_BlockHeap.cpp
	coreinit_MEM_BlockHeap.h
	coreinit_MEM_ExpHeap.cpp
	coreinit_MEM_ExpHeap.h
	coreinit_MEM_FrmHeap.cpp
	coreinit_MEM_FrmHeap.h
	coreinit_MEM_UnitHeap.cpp
	coreinit_MEM_UnitHeap.h
	coreinit_Memory.cpp
	coreinit_Memory.h
	coreinit_MemoryMapping.cpp
	coreinit_MemoryMapping.h
	coreinit_MessageQueue.cpp
	coreinit_MessageQueue.h
	coreinit_Misc.cpp
	coreinit_Misc.h
	coreinit_MPQueue.cpp
	coreinit_MPQueue.h
	coreinit_OSScreen.cpp
	coreinit_OSScreen.h
	coreinit_OSScreen_font.h
	coreinit_OverlayArena.cpp
	coreinit_OverlayArena.h
	coreinit_Scheduler.cpp
	coreinit_Scheduler.h
	coreinit_Spinlock.cpp
	coreinit_Spinlock.h
	coreinit_Synchronization.cpp
	coreinit_SysHeap.cpp
	coreinit_SysHeap.h
	coreinit_SystemInfo.cpp
	coreinit_SystemInfo.h
	coreinit_Thread.cpp
	coreinit_Thread.h
	coreinit_ThreadQueue.cpp
	coreinit_Time.cpp
	coreinit_Time.h
)
pixelu_add_folder(CemuCafe OS/libs/dmae dmae.cpp dmae.h)
pixelu_add_folder(CemuCafe OS/libs/drmapp drmapp.cpp drmapp.h)
pixelu_add_folder(CemuCafe OS/libs/erreula erreula.cpp erreula.h)
pixelu_add_folder(CemuCafe OS/libs/gx2
	GX2.cpp
	GX2.h
	GX2_AddrTest.cpp
	GX2_Blit.cpp
	GX2_Blit.h
	GX2_Command.cpp
	GX2_Command.h
	GX2_ContextState.cpp
	GX2_Draw.cpp
	GX2_Draw.h
	GX2_Event.cpp
	GX2_Event.h
	GX2_Memory.cpp
	GX2_Memory.h
	GX2_Misc.cpp
	GX2_Misc.h
	GX2_Query.cpp
	GX2_Query.h
	GX2_RenderTarget.cpp
	GX2_Resource.cpp
	GX2_Resource.h
	GX2_Shader.cpp
	GX2_Shader.h
	GX2_shader_legacy.cpp
	GX2_State.cpp
	GX2_State.h
	GX2_Streamout.cpp
	GX2_Streamout.h
	GX2_Surface.cpp
	GX2_Surface.h
	GX2_Surface_Copy.cpp
	GX2_Surface_Copy.h
	GX2_Texture.cpp
	GX2_Texture.h
	GX2_TilingAperture.cpp
)
pixelu_add_folder(CemuCafe OS/libs/h264_avc
	H264Dec.cpp
	h264dec.h
	H264DecBackendAVC.cpp
	H264DecInternal.h
)
pixelu_add_folder(CemuCafe OS/libs/h264_avc/parser H264Parser.cpp H264Parser.h)
pixelu_add_folder(CemuCafe OS/libs/mic mic.cpp mic.h)
pixelu_add_folder(CemuCafe OS/libs/nfc
	ndef.cpp
	ndef.h
	nfc.cpp
	nfc.h
	stream.cpp
	stream.h
	TagV0.cpp
	TagV0.h
	TLV.cpp
	TLV.h
)
pixelu_add_folder(CemuCafe OS/libs/nlibcurl nlibcurl.cpp nlibcurl.h nlibcurlDebug.hpp)
pixelu_add_folder(CemuCafe OS/libs/nlibnss nlibnss.cpp nlibnss.h)
pixelu_add_folder(CemuCafe OS/libs/nn_ac nn_ac.cpp nn_ac.h)
pixelu_add_folder(CemuCafe OS/libs/nn_acp nn_acp.cpp nn_acp.h)
pixelu_add_folder(CemuCafe OS/libs/nn_act nn_act.cpp nn_act.h)
pixelu_add_folder(CemuCafe OS/libs/nn_aoc nn_aoc.cpp nn_aoc.h)
pixelu_add_folder(CemuCafe OS/libs/nn_boss nn_boss.cpp nn_boss.h)
pixelu_add_folder(CemuCafe OS/libs/nn_ccr nn_ccr.cpp nn_ccr.h)
pixelu_add_folder(CemuCafe OS/libs/nn_cmpt nn_cmpt.cpp nn_cmpt.h)
pixelu_add_folder(CemuCafe OS/libs/nn_ec nn_ec.cpp nn_ec.h)
pixelu_add_folder(CemuCafe OS/libs/nn_fp nn_fp.cpp nn_fp.h)
pixelu_add_folder(CemuCafe OS/libs/nn_idbe nn_idbe.cpp nn_idbe.h)
pixelu_add_folder(CemuCafe OS/libs/nn_ndm nn_ndm.cpp nn_ndm.h)
pixelu_add_folder(CemuCafe OS/libs/nn_nfp AmiiboCrypto.h nn_nfp.cpp nn_nfp.h)
pixelu_add_folder(CemuCafe OS/libs/nn_nim nn_nim.cpp nn_nim.h)
pixelu_add_folder(CemuCafe OS/libs/nn_olv
	nn_olv.cpp
	nn_olv.h
	nn_olv_Common.cpp
	nn_olv_Common.h
	nn_olv_DownloadCommunityTypes.cpp
	nn_olv_DownloadCommunityTypes.h
	nn_olv_InitializeTypes.cpp
	nn_olv_InitializeTypes.h
	nn_olv_OfflineDB.cpp
	nn_olv_OfflineDB.h
	nn_olv_PostTypes.cpp
	nn_olv_PostTypes.h
	nn_olv_UploadCommunityTypes.cpp
	nn_olv_UploadCommunityTypes.h
	nn_olv_UploadFavoriteTypes.cpp
	nn_olv_UploadFavoriteTypes.h
)
pixelu_add_folder(CemuCafe OS/libs/nn_pdm nn_pdm.cpp nn_pdm.h)
pixelu_add_folder(CemuCafe OS/libs/nn_save nn_save.cpp nn_save.h)
pixelu_add_folder(CemuCafe OS/libs/nn_sl nn_sl.cpp nn_sl.h)
pixelu_add_folder(CemuCafe OS/libs/nn_spm nn_spm.cpp nn_spm.h)
pixelu_add_folder(CemuCafe OS/libs/nn_temp nn_temp.cpp nn_temp.h)
pixelu_add_folder(CemuCafe OS/libs/nn_uds nn_uds.cpp nn_uds.h)
pixelu_add_folder(CemuCafe OS/libs/nsyshid
	AttachDefaultBackends.cpp
	Backend.h
	BackendEmulated.cpp
	BackendEmulated.h
	Dimensions.cpp
	Dimensions.h
	Infinity.cpp
	Infinity.h
	nsyshid.cpp
	nsyshid.h
	Skylander.cpp
	Skylander.h
	SkylanderXbox360.cpp
	SkylanderXbox360.h
	Whitelist.cpp
	Whitelist.h
)
pixelu_add_folder(CemuCafe OS/libs/nsyshid/g721 g721.cpp g721.h)
pixelu_add_folder(CemuCafe OS/libs/nsyskbd nsyskbd.cpp nsyskbd.h)
pixelu_add_folder(CemuCafe OS/libs/nsysnet nsysnet.cpp nsysnet.h)
pixelu_add_folder(CemuCafe OS/libs/ntag ntag.cpp ntag.h)
pixelu_add_folder(CemuCafe OS/libs/padscore padscore.cpp padscore.h)
pixelu_add_folder(CemuCafe OS/libs/proc_ui proc_ui.cpp proc_ui.h)
pixelu_add_folder(CemuCafe OS/libs/snd_core
	ax.h
	ax_aux.cpp
	ax_exports.cpp
	ax_internal.h
	ax_ist.cpp
	ax_mix.cpp
	ax_multivoice.cpp
	ax_out.cpp
	ax_voice.cpp
)
pixelu_add_folder(CemuCafe OS/libs/snd_user snd_user.cpp snd_user.h)
pixelu_add_folder(CemuCafe OS/libs/swkbd swkbd.cpp swkbd.h)
pixelu_add_folder(CemuCafe OS/libs/sysapp sysapp.cpp sysapp.h)
pixelu_add_folder(CemuCafe OS/libs/TCL TCL.cpp TCL.h)
pixelu_add_folder(CemuCafe OS/libs/vpad vpad.cpp vpad.h)
pixelu_add_folder(CemuCafe OS/libs/zlib125 zlib125.cpp zlib125.h)
pixelu_add_folder(CemuCafe OS/RPL
	COSModule.cpp
	COSModule.h
	elf.cpp
	rpl.cpp
	rpl.h
	rpl_debug_symbols.cpp
	rpl_debug_symbols.h
	rpl_structs.h
	rpl_symbol_storage.cpp
	rpl_symbol_storage.h
)
pixelu_add_folder(CemuCafe TitleList
	AppType.h
	GameInfo.cpp
	GameInfo.h
	ParsedMetaXml.h
	SaveInfo.cpp
	SaveInfo.h
	SaveList.cpp
	SaveList.h
	TitleId.h
	TitleInfo.cpp
	TitleInfo.h
	TitleList.cpp
	TitleList.h
)

if(ENABLE_OPENGL)
	pixelu_add_folder(CemuCafe HW/Latte/Renderer/OpenGL
		CachedFBOGL.h
		LatteShaderGL.cpp
		LatteTextureGL.cpp
		LatteTextureGL.h
		LatteTextureViewGL.cpp
		LatteTextureViewGL.h
		OpenGLQuery.cpp
		OpenGLRecorderCapture.cpp
		OpenGLRenderer.cpp
		OpenGLRenderer.h
		OpenGLRendererCore.cpp
		OpenGLRendererStreamout.cpp
		OpenGLRendererUniformData.cpp
		OpenGLSurfaceCopy.cpp
		OpenGLTextureReadback.h
		RendererShaderGL.cpp
		RendererShaderGL.h
		TextureReadbackGL.cpp
	)
endif()

if(ENABLE_VULKAN)
	pixelu_add_folder(CemuCafe HW/Latte/Renderer/Vulkan
		CachedFBOVk.cpp
		CachedFBOVk.h
		CocoaSurface.h
		LatteTextureViewVk.cpp
		LatteTextureViewVk.h
		LatteTextureVk.cpp
		LatteTextureVk.h
		RendererShaderVk.cpp
		RendererShaderVk.h
		SwapchainInfoVk.cpp
		SwapchainInfoVk.h
		TextureReadbackVk.cpp
		VKRBase.h
		VKRMemoryManager.cpp
		VKRMemoryManager.h
		VKRPipelineInfo.cpp
		VsyncDriver.cpp
		VsyncDriver.h
		VulkanAPI.cpp
		VulkanAPI.h
		VulkanPipelineCompiler.cpp
		VulkanPipelineCompiler.h
		VulkanPipelineStableCache.cpp
		VulkanPipelineStableCache.h
		VulkanQuery.cpp
		VulkanRecorderCapture.cpp
		VulkanRenderer.cpp
		VulkanRenderer.h
		VulkanRendererCore.cpp
		VulkanSurfaceCopy.cpp
		VulkanTextureReadback.h
	)
endif()

if(APPLE)
	# view with a CAMetalLayer, the surface MoltenVK draws to
	pixelu_add_folder(CemuCafe HW/Latte/Renderer MetalView.h MetalView.mm)
	target_link_libraries(CemuCafe PRIVATE "-framework QuartzCore")
	if(ENABLE_VULKAN)
		pixelu_add_folder(CemuCafe HW/Latte/Renderer/Vulkan CocoaSurface.mm)
	endif()
endif()

if(CEMU_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
	pixelu_add_folder(CemuCafe HW/Espresso/Recompiler/BackendAArch64 BackendAArch64.cpp BackendAArch64.h)
	target_link_libraries(CemuCafe PRIVATE xbyak_aarch64)
endif()

pixelu_library(CemuCafe)

if(ENABLE_LIBUSB)
	pixelu_add_folder(CemuCafe OS/libs/nsyshid BackendLibusb.cpp BackendLibusb.h)
endif()

target_include_directories(CemuCafe PUBLIC "../")

# glslang 15 merged the SPIRV library into glslang
if(glslang_VERSION VERSION_LESS "15.0.0")
	set(PIXELU_GLSLANG glslang::SPIRV)
else()
	set(PIXELU_GLSLANG glslang::glslang)
endif()

target_link_libraries(CemuCafe
	PRIVATE
		CemuCommon
		CemuGui
		ZArchive::zarchive
		imguiImpl
		pugixml::pugixml
		ZLIB::ZLIB
		CURL::libcurl
		ih264d
		zstd::zstd
		${PIXELU_GLSLANG}
	PUBLIC
		OpenSSL::SSL
)

if(ENABLE_WAYLAND)
	# public: VulkanAPI.h includes wayland-client.h
	target_link_libraries(CemuCafe PUBLIC Wayland::Client)
endif()

if(ENABLE_LIBUSB)
	if(ENABLE_VCPKG)
		# vcpkg's libusb comes with a pkg-config file only
		if(WIN32)
			set(PKG_CONFIG_EXECUTABLE "${VCPKG_INSTALLED_DIR}/x64-windows/tools/pkgconf/pkgconf.exe")
		endif()
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)
		target_link_libraries(CemuCafe PRIVATE PkgConfig::libusb)
	else()
		find_package(libusb MODULE REQUIRED)
		target_link_libraries(CemuCafe PRIVATE libusb::libusb)
	endif()
endif()

if(WIN32)
	target_link_libraries(CemuCafe PRIVATE iphlpapi) # network adapter queries
endif()
