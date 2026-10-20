// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Abstract graphics backend. Latte (the GPU emulation) talks to the host GPU only through this
// interface; OpenGLRenderer and VulkanRenderer implement it.

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"
#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"
#include "Cafe/HW/Latte/Core/LatteQueryObject.h"
#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"

#if BOOST_OS_WINDOWS
#include "util/DXGIWrapper/DXGIWrapper.h"
#endif

struct ImFontAtlas;
struct ImGuiContext;

enum class GfxVendor
{
	Generic,
	AMD,
	Intel,
	Nvidia,
	Apple,
	Mesa,
	MAX
};

enum class RendererAPI
{
	OpenGL,
	Vulkan,
	Metal,
	MAX
};

using ImTextureID = void*;

class Renderer
{
public:
	enum class INDEX_TYPE
	{
		NONE,
		U16,
		U32
	};

	struct BindBufferParam
	{
		uint8 index;
		uint16 stride;
		uint32 bindOffset;
		uint32 bindSize;
	};

	struct IndexAllocation
	{
		void* mem; // CPU-visible destination for the index data
		void* rendererInternal; // backend bookkeeping
	};

	// receives RGB8 pixels, width, height and whether the TV picture was captured; returns an optional overlay message
	using ScreenshotSaveFunction = std::function<std::optional<std::string>(const std::vector<uint8>&, int, int, bool)>;

	Renderer(RendererAPI api) : m_rendererAPI(api) {}
	virtual ~Renderer() = default;

	RendererAPI GetType() const { return m_rendererAPI; }
	const std::string& GetDeviceName() { return m_selectedDeviceName; }
	GfxVendor GetVendor() const { return m_vendor; }

	// lifetime
	virtual void Initialize();
	virtual void Shutdown();
	virtual bool IsPadWindowActive() = 0;
	virtual bool GetVRAMInfo(int& usageInMB, int& totalInMB) const;
	virtual void EnableDebugMode() {}

	// frame presentation
	virtual void ClearColorbuffer(bool padView) = 0;
	virtual void DrawEmptyFrame(bool mainWindow) = 0;
	virtual void SwapBuffers(bool swapTV, bool swapDRC) = 0;
	virtual void DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
									sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
									bool padView, bool clearBackground) = 0;
	virtual bool BeginFrame(bool mainWindow) = 0;

	// screenshots
	void RequestScreenshot(ScreenshotSaveFunction onSaveScreenshot);
	void CancelScreenshotRequest();
	virtual void HandleScreenshotRequest(LatteTextureView* texView, bool padView) {}

	// PixelU recorder (see LatteRecorder.h): recorder_captureTVFrame queues a readback of the TV picture without
	// waiting for the GPU, recorder_pollFrames hands finished readbacks to LatteRecorder::SubmitVideoFrame
	virtual void recorder_captureTVFrame(LatteTextureView* texView, sint64 frameSlot) {}
	virtual void recorder_pollFrames() {}

	// submission control
	virtual void Flush(bool waitIdle = false) = 0; // explicit flush, e.g. requested by the overlay
	virtual void NotifyLatteCommandProcessorIdle() = 0; // the command processor ran out of work or is stalled
	virtual void SurfaceSync(Latte::E_COHER_CNTL coher, MPTR address, uint32 size) {} // GX2Invalidate hint, optional

	// imgui overlay
	virtual bool ImguiBegin(bool mainWindow);
	virtual void ImguiEnd() = 0;
	virtual ImTextureID GenerateTexture(const std::vector<uint8>& data, const Vector2i& size) = 0;
	virtual void DeleteTexture(ImTextureID id) = 0;
	virtual void DeleteFontTextures() = 0;
	virtual void AppendOverlayDebugInfo() = 0;

	virtual bool UseTFViaSSBO() const { return false; }

	// render targets
	virtual void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ = false) = 0;
	virtual void renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight) = 0;
	virtual LatteCachedFBO* rendertarget_createCachedFBO(uint64 key) = 0;
	virtual void rendertarget_deleteCachedFBO(LatteCachedFBO* fbo) = 0;
	virtual void rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo) = 0;

	// textures
	virtual void* texture_acquireTextureUploadBuffer(uint32 size) = 0;
	virtual void texture_releaseTextureUploadBuffer(uint8* mem) = 0;
	virtual TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) = 0;
	virtual void texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex) = 0;
	virtual void texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize) = 0;
	virtual void texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a) = 0;
	virtual void texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue) = 0;
	virtual LatteTexture* texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth) = 0;
	virtual void texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit) = 0;
	virtual void texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth) = 0;
	virtual LatteTextureReadbackInfo* texture_createReadback(LatteTextureView* textureView) = 0;

	// surface copy with format conversion (e.g. depth <-> color)
	virtual void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height) = 0;

	// the big buffer that mirrors vertex/uniform data
	virtual void bufferCache_init(const sint32 bufferSize) = 0;
	virtual void bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset) = 0;
	virtual void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) = 0;
	virtual void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) = 0;
	virtual void buffer_bindVertexBuffers(std::span<BindBufferParam> bindings) = 0;
	virtual void buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size) = 0;

	// shaders
	virtual RendererShader* shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool compileAsync, bool isGfxPackSource) = 0;

	// streamout (transform feedback)
	virtual void streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize) = 0;
	virtual void streamout_begin() = 0;
	virtual void streamout_rendererFinishDrawcall() = 0;

	// draw calls
	virtual void draw_beginSequence() = 0;
	virtual void draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext) = 0;
	virtual void draw_endSequence() = 0;

	// index data
	virtual IndexAllocation indexData_reserveIndexMemory(uint32 size) = 0;
	virtual void indexData_releaseIndexMemory(IndexAllocation& allocation) = 0;
	virtual void indexData_uploadIndexMemory(IndexAllocation& allocation) = 0;

	// occlusion queries
	virtual LatteQueryObject* occlusionQuery_create() = 0;
	virtual void occlusionQuery_destroy(LatteQueryObject* queryObj) = 0;
	virtual void occlusionQuery_flush() = 0;
	virtual void occlusionQuery_updateState() = 0;

protected:
	enum class ScreenshotState
	{
		None,
		Main,
		Pad,
	};

	virtual void GetVendorInformation() {}

	// 8-bit sRGB <-> linear conversion used when a screenshot's colour space differs from the scan buffer's
	static uint8 SRGBComponentToRGB(uint8 ci);
	static uint8 RGBComponentToSRGB(uint8 cli);

	// hands the pixels to the save callback on a worker thread and clears the pending request
	void SaveScreenshot(const std::vector<uint8>& rgb_data, int width, int height, bool mainWindow);

	RendererAPI m_rendererAPI;
	GfxVendor m_vendor = GfxVendor::Generic;
	std::string m_selectedDeviceName = "";

	ScreenshotState m_screenshot_state = ScreenshotState::None;
	bool m_screenshot_requested = false;
	ScreenshotSaveFunction m_on_save_screenshot;

	ImFontAtlas* imguiFontAtlas{};
	ImGuiContext* imguiTVContext{};
	ImGuiContext* imguiPadContext{};

#if BOOST_OS_WINDOWS
	std::unique_ptr<DXGIWrapper> m_dxgi_wrapper{};
#endif
};

extern std::unique_ptr<Renderer> g_renderer;
