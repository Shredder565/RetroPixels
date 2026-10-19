// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Common/GLInclude/GLInclude.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"

// vertex attribute slots used on the host; GPU7 has more, but 16 is what every desktop driver offers
#define GPU_GL_MAX_NUM_ATTRIBUTE		(16)

// The window system (wx canvas) owns the GL contexts. It installs these callbacks so the renderer can switch
// between the TV and GamePad windows and present them.
class OpenGLCanvasCallbacks
{
public:
	virtual bool HasPadViewOpen() const { return false; }
	virtual bool MakeCurrent(bool padView) { return false; }
	virtual void SwapBuffers(bool swapTV, bool swapDRC) {}
	virtual ~OpenGLCanvasCallbacks() = default;
};

void SetOpenGLCanvasCallbacks(OpenGLCanvasCallbacks* callbacks);
void ClearOpenGLCanvasCallbacks();

bool GLCanvas_HasPadViewOpen();
bool GLCanvas_MakeCurrent(bool padView);
void GLCanvas_SwapBuffers(bool swapTV, bool swapDRC);

class OpenGLRenderer : public Renderer
{
	friend class OpenGLCanvas;
public:
	OpenGLRenderer();
	~OpenGLRenderer();

	static OpenGLRenderer* GetInstance();

	// overlay
	bool ImguiBegin(bool mainWindow) override;
	void ImguiEnd() override;
	ImTextureID GenerateTexture(const std::vector<uint8>& data, const Vector2i& size) override;
	void DeleteTexture(ImTextureID id) override;
	void DeleteFontTextures() override;
	void AppendOverlayDebugInfo() override {}

	void Initialize() override;
	bool IsPadWindowActive() override;

	void Flush(bool waitIdle = false) override;
	void NotifyLatteCommandProcessorIdle() override;
	void EnableDebugMode() override;

	// presentation
	void SwapBuffers(bool swapTV = true, bool swapDRC = true) override;
	bool BeginFrame(bool mainWindow) override;
	void DrawEmptyFrame(bool mainWindow) override;
	void ClearColorbuffer(bool padView) override;
	void HandleScreenshotRequest(LatteTextureView* texView, bool padView) override;
	void DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter, sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground) override;

	// PixelU recorder (OpenGLRecorderCapture.cpp)
	void recorder_captureTVFrame(LatteTextureView* texView, sint64 frameSlot) override;
	void recorder_pollFrames() override;

	// render targets
	void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ = false) override;
	void renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight) override;
	LatteCachedFBO* rendertarget_createCachedFBO(uint64 key) override;
	void rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo) override;

	// fixed function state
	void renderstate_setChannelTargetMask(uint32 renderTargetMask);
	void renderstate_setAlwaysWriteDepth();
	void renderstate_updateBlendingAndColorControl();
	void renderstate_resetColorControl();
	void renderstate_resetDepthControl();
	void renderstate_resetStencilMask();
	// GL keeps sampler state in the texture (view) object; applies swizzle and sampler registers of one texture unit
	void renderstate_updateTextureSettingsGL(LatteDecompilerShader* shaderContext, LatteTextureView* _hostTextureView, uint32 hostTextureUnit, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N texUnitWord4, uint32 texUnitIndex, bool isDepthSampler);

	// textures
	void* texture_acquireTextureUploadBuffer(uint32 size) override;
	void texture_releaseTextureUploadBuffer(uint8* mem) override;
	bool SupportsS3TC(bool isSRGB) const { return isSRGB ? m_supportsS3TCSRGB : m_supportsS3TC; }
	TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) override;
	void texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex) override;
	void texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize) override;
	void texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a) override;
	void texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue) override;
	LatteTexture* texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth) override;
	void texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit) override;
	void texture_bindAndActivate(LatteTextureView* textureView, uint32 textureUnit);
	void texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth) override;
	void texture_notifyDelete(LatteTextureView* textureView);
	LatteTextureReadbackInfo* texture_createReadback(LatteTextureView* textureView) override;

	void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height) override;

	// buffer cache and vertex attributes
	void attributeStream_reset();
	void bufferCache_init(const sint32 bufferSize) override;
	void bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset) override;
	void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void buffer_bindVertexBuffers(std::span<BindBufferParam> bindings) override;
	void buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size) override;
	void _setupVertexAttributes();
	void attributeStream_bindVertexCacheBuffer();
	void attributeStream_unbindVertexBuffer();
	static void SetAttributeArrayState(uint32 index, bool isEnabled, sint32 aluDivisor);
	static void SetArrayElementBuffer(GLuint arrayElementBuffer);

	// the GL backend caches index data itself and does not use these
	IndexAllocation indexData_reserveIndexMemory(uint32 size) override
	{
		cemu_assert_unimplemented();
		return {};
	}
	void indexData_releaseIndexMemory(IndexAllocation& allocation) override { cemu_assert_unimplemented(); }
	void indexData_uploadIndexMemory(IndexAllocation& allocation) override { cemu_assert_unimplemented(); }

	// uniforms
	void uniformData_update();

	// shaders
	RendererShader* shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackShader) override;
	void shader_bind(RendererShader* shader);
	void shader_unbind(RendererShader::ShaderType shaderType);

	// streamout
	void streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize) override;
	void streamout_begin() override;
	void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void streamout_rendererFinishDrawcall() override;

	// draw calls
	void draw_init();
	void draw_beginSequence() override;
	void draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext) override;
	void draw_endSequence() override;
	template<bool TIsMinimal, bool THasProfiling>
	void draw_genericDrawHandler(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType);

	// end of frame housekeeping
	void cleanupAfterFrame();
	void ReleaseBufferCacheEntries();

	// occlusion queries
	LatteQueryObject* occlusionQuery_create() override;
	void occlusionQuery_destroy(LatteQueryObject* queryObj) override;
	void occlusionQuery_flush() override;
	void occlusionQuery_updateState() override {}

private:
	void GetVendorInformation() override;
	void texture_setActiveTextureUnit(sint32 index);
	// copies the GL backend cannot do with glCopyImageSubData
	void texture_syncSliceSpecialBC4(LatteTexture* srcTexture, sint32 srcSliceIndex, sint32 srcMipIndex, LatteTexture* dstTexture, sint32 dstSliceIndex, sint32 dstMipIndex);
	void texture_syncSliceSpecialIntegerToBC3(LatteTexture* srcTexture, sint32 srcSliceIndex, sint32 srcMipIndex, LatteTexture* dstTexture, sint32 dstSliceIndex, sint32 dstMipIndex);
	void ApplyStencilState();
	void ApplyBlendState();
	void ApplyRasterizerState();
	void SetBlendEnableMask(uint32 mask);

	// PixelU recorder readback slots (OpenGLRecorderCapture.cpp)
	struct RecorderReadback
	{
		GLuint pixelBuffer = 0;
		size_t bufferSize = 0;
		GLsync fence = nullptr;
		bool pending = false;
		sint64 frameSlot = 0;
		uint32 width = 0;
		uint32 height = 0;
		LatteRecorder::ColorConversion colorConversion = LatteRecorder::ColorConversion::None;
	};
	std::array<RecorderReadback, 4> m_recorderReadbacks{};

	GLuint m_pipeline = 0; // program pipeline holding the separable stage programs
	bool m_isPadViewContext{};
	bool m_supportsS3TC = false;
	bool m_supportsS3TCSRGB = false;
	bool m_isXfbActive = false;

	// last applied values; initial values that cannot match force the first update
	struct
	{
		float x = 0, y = 0, width = 0, height = 0;
		float nearZ = -999999.0f, farZ = -9999999.0f;
		bool invertY = false;
		bool halfZ = false;
		bool scissorEnabled = false;
	} m_viewState;

	struct
	{
		bool depthTest = false;
		bool depthWrite = false;
		Latte::LATTE_DB_DEPTH_CONTROL::E_ZFUNC depthFunc = (Latte::LATTE_DB_DEPTH_CONTROL::E_ZFUNC)-1;
		bool stencilTest = false;
		struct Face
		{
			Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC func = (Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC)-1;
			Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION opFail = (Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION)-1;
			Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION opZFail = (Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION)-1;
			Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION opZPass = (Latte::LATTE_DB_DEPTH_CONTROL::E_STENCILACTION)-1;
			uint32 ref = 0xFFFFFFFF;
			uint32 compareMask = 0xFFFFFFFF;
			uint32 writeMask = 0xFFFFFFFF;
		} front, back;
	} m_depthStencilState;

	struct
	{
		uint32 colorWriteMask = 0; // 4 bits (RGBA) per target, target 0 in the lowest bits
		uint32 enableMask = 0;
		uint32 controlReg[8]{};
		uint32 constantColor[4]{};
		uint32 logicOp = 0;
		bool alphaTest = false;
	} m_blendState;

	struct
	{
		uint32 cullEnable = 0;
		uint32 cullFront = 0;
		uint32 cullBack = 0;
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace = Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW;
		uint32 offsetScaleReg = 0;
		uint32 offsetOffsetReg = 0;
		uint32 offsetClampReg = 0;
		uint32 offsetFillEnable = 0;
		uint32 zClipEnable = 0;
		uint32 pointSizeReg = 0xFFFFFFFF;
		uint32 restartIndex = 0xFFFFFFFF;
	} m_rasterState;

	struct
	{
		GLuint vertex = (GLuint)-1;
		GLuint geometry = (GLuint)-1;
		GLuint fragment = (GLuint)-1;
	} m_boundPrograms;

	// texture units
	sint32 activeTextureUnit = 0;
	void* m_latteBoundTextures[Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE * 3]{};

	GLuint m_backbufferBlit_uniformBuffer = 0;
	GLuint glAttributeCacheAB{}; // vertex and uniform data mirror
	GLuint _boundArrayBuffer{};
	GLuint glStreamoutCacheRingBuffer = 0;
	GLuint prevBoundFBO = 0;

	std::vector<class LatteQueryObjectGL*> list_queryCacheOcclusion; // recycled queries
};
