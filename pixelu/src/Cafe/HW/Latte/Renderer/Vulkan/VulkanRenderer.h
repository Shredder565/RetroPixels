// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/CachedFBOVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/SwapchainInfoVk.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"
#include "util/math/vector2.h"
#include "util/helpers/Semaphore.h"
#include "util/containers/flat_hash_map.hpp"
#include "util/containers/robin_hood.h"

// optional packed formats, probed once at device creation
struct VkSupportedFormatInfo_t
{
	bool fmt_d24_unorm_s8_uint{};
	bool fmt_r4g4_unorm_pack{};
	bool fmt_r5g6b5_unorm_pack{};
	bool fmt_r4g4b4a4_unorm_pack{};
	bool fmt_a1r5g5b5_unorm_pack{};
};

// a descriptor set built for one shader stage of one pipeline, cached by the state hash of its bindings
struct VkDescriptorSetInfo
{
	~VkDescriptorSetInfo();

	VKRObjectDescriptorSet* m_vkObjDescriptorSet{};
	std::vector<LatteTextureViewVk*> list_referencedViews;
	std::vector<LatteTextureVk*> list_fboCandidates; // bound textures that might also be render targets (barrier candidates)
	LatteConst::ShaderType shaderType{};
	uint64 stateHash{};
	class PipelineInfo* pipeline_info{};

	// counts added to the performance monitor, removed again on destruction
	uint8 statsNumSamplerTextures{ 0 };
	uint8 statsNumDynUniformBuffers{ 0 };
	uint8 statsNumStorageBuffers{ 0 };
};

class VkException : public std::runtime_error
{
public:
	VkException(VkResult result, const std::string& message) : runtime_error(message), m_result(result) {}
	VkResult GetResult() const { return m_result; }

private:
	VkResult m_result;
};

namespace VulkanRendererConst
{
	// per stage arrays are indexed directly by LatteConst::ShaderType
	static const inline int SHADER_STAGE_INDEX_VERTEX = static_cast<int>(LatteConst::ShaderType::Vertex);
	static const inline int SHADER_STAGE_INDEX_FRAGMENT = static_cast<int>(LatteConst::ShaderType::Pixel);
	static const inline int SHADER_STAGE_INDEX_GEOMETRY = static_cast<int>(LatteConst::ShaderType::Geometry);
	static const inline int SHADER_STAGE_INDEX_COUNT = 4;
};

static_assert(static_cast<int>(LatteConst::ShaderType::Vertex) == 0);
static_assert(static_cast<int>(LatteConst::ShaderType::Pixel) == 1);
static_assert(static_cast<int>(LatteConst::ShaderType::Geometry) == 2);

// one graphics pipeline (shader combination + fixed function state) and the descriptor sets created for it
class PipelineInfo
{
public:
	PipelineInfo(uint64 minimalStateHash, uint64 pipelineHash, struct LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* pixelShader, LatteDecompilerShader* geometryShader);
	PipelineInfo(const PipelineInfo& info) = delete;
	~PipelineInfo();

	bool operator==(const PipelineInfo&) const { return true; }

	// the keys are hashes already, so they are used as bucket values unchanged
	template<typename T>
	struct direct_hash
	{
		size_t operator()(const uint64& k) const noexcept { return k; }
	};
	using DescriptorSetCache = ska::flat_hash_map<uint64, VkDescriptorSetInfo*, direct_hash<uint64>>;

	FORCE_INLINE DescriptorSetCache& GetDescriptorSetCache(LatteConst::ShaderType shaderType)
	{
		cemu_assert_debug(shaderType == LatteConst::ShaderType::Vertex || shaderType == LatteConst::ShaderType::Pixel || shaderType == LatteConst::ShaderType::Geometry);
		return ds_cache[static_cast<size_t>(shaderType)];
	}

	DescriptorSetCache ds_cache[VulkanRendererConst::SHADER_STAGE_INDEX_COUNT];

	VKRObjectPipeline* m_vkrObjPipeline;

	LatteDecompilerShader* vertexShader = nullptr;
	LatteDecompilerShader* geometryShader = nullptr;
	LatteDecompilerShader* pixelShader = nullptr;
	LatteFetchShader* fetchShader = nullptr;
	Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE primitiveMode{};

	RendererShaderVk* vertexShaderVk = nullptr;
	RendererShaderVk* geometryShaderVk = nullptr;
	RendererShaderVk* pixelShaderVk = nullptr;

	uint64 minimalStateHash;
	uint64 stateHash;

	bool usesBlendConstants{ false };
	bool usesDepthBias{ false };

	struct
	{
		bool hasUniformVar[VulkanRendererConst::SHADER_STAGE_INDEX_COUNT];
		bool hasUniformBuffers[VulkanRendererConst::SHADER_STAGE_INDEX_COUNT];
		std::vector<uint8> list_uniformBuffers[VulkanRendererConst::SHADER_STAGE_INDEX_COUNT];
	}dynamicOffsetInfo{};

	// geometry shader that expands RECT primitives into two triangles
	RendererShaderVk* rectEmulationGS = nullptr;

	// pipelines known to need the full draw barrier even when it would normally be skipped
	bool neverSkipAccurateBarrier{ false };
};

namespace WindowSystem
{
	struct WindowHandleInfo;
};

class VulkanRenderer : public Renderer
{
	friend class LatteQueryObjectVk;
	friend class LatteTextureReadbackInfoVk;
	friend class PipelineCompiler;

	using VSync = SwapchainInfoVk::VSync;

	static const inline int UNIFORMVAR_RINGBUFFER_SIZE = 16 * 1024 * 1024;
	static const inline int TEXTURE_READBACK_SIZE = 32 * 1024 * 1024;
	static const inline int OCCLUSION_QUERY_POOL_SIZE = 1024;

public:
	std::unique_ptr<VKRMemoryManager> memoryManager;
	VKRMemoryManager* GetMemoryManager() const { return memoryManager.get(); };

	VkSupportedFormatInfo_t m_supportedFormatInfo;

	// host representation of a guest surface format
	typedef struct
	{
		VkFormat vkImageFormat;
		VkImageAspectFlags vkImageAspect;
		bool isCompressed;
		bool isAlternateFormat; // the host format differs from the guest layout, data goes through a converting decoder
		TextureDecoder* decoder;
		sint32 texelCountX;
		sint32 texelCountY;
	}FormatInfoVK;

	// entry of the GPU selection list in the settings
	struct DeviceInfo
	{
		DeviceInfo(const std::string name, std::span<uint8, VK_UUID_SIZE> uuid) : name(name)
		{
			std::copy(uuid.begin(), uuid.end(), this->uuid.begin());
		}

		std::string name;
		std::array<uint8, VK_UUID_SIZE> uuid;
	};

	static std::vector<DeviceInfo> GetDevices();
	VulkanRenderer();
	virtual ~VulkanRenderer();

	static VulkanRenderer* GetInstance();

	void UnrecoverableError(const char* errMsg) const;

	void GetDeviceFeatures();
	void DetermineVendor();
	void InitializeSurface(const Vector2i& size, bool mainWindow);

	const std::unique_ptr<SwapchainInfoVk>& GetChainInfoPtr(bool mainWindow) const;
	SwapchainInfoVk& GetChainInfo(bool mainWindow) const;

	// called from the GamePad window when it closes: the next acquire on the pad swapchain tears it down and wakes the caller
	void StopUsingPadAndWait();
	bool IsPadWindowActive() override;

	void HandleScreenshotRequest(LatteTextureView* texView, bool padView) override;

	// PixelU recorder (VulkanRecorderCapture.cpp)
	void recorder_captureTVFrame(LatteTextureView* texView, sint64 frameSlot) override;
	void recorder_pollFrames() override;
private:
	struct RecorderReadback
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint8* mappedPtr = nullptr;
		size_t bufferSize = 0;
		uint64 commandBufferId = 0;
		bool pending = false;
		sint64 frameSlot = 0;
		uint32 width = 0;
		uint32 height = 0;
		LatteRecorder::ColorConversion colorConversion = LatteRecorder::ColorConversion::None;
	};
	std::array<RecorderReadback, 4> m_recorderReadbacks{};
	VkImage m_recorderBlitImage = VK_NULL_HANDLE;
	VkDeviceMemory m_recorderBlitImageMemory = VK_NULL_HANDLE;
	VkFormat m_recorderBlitFormat = VK_FORMAT_UNDEFINED;
	uint32 m_recorderBlitWidth = 0;
	uint32 m_recorderBlitHeight = 0;
	void recorder_waitForPendingReadbacks();
	bool recorder_prepareBlitImage(VkFormat format, uint32 width, uint32 height);
	bool recorder_prepareReadbackBuffer(RecorderReadback& readback, size_t size);
	void recorder_releaseResources();
public:

	void QueryMemoryInfo();
	void QueryAvailableFormats();

	// window system surfaces
#if BOOST_OS_WINDOWS
	static VkSurfaceKHR CreateWinSurface(VkInstance instance, HWND hwindow);
#endif
#if BOOST_OS_LINUX || BOOST_OS_BSD
	static VkSurfaceKHR CreateXlibSurface(VkInstance instance, Display* dpy, Window window);
	static VkSurfaceKHR CreateXcbSurface(VkInstance instance, xcb_connection_t* connection, xcb_window_t window);
#ifdef HAS_WAYLAND
	static VkSurfaceKHR CreateWaylandSurface(VkInstance instance, wl_display* display, wl_surface* surface);
#endif
#endif
	static VkSurfaceKHR CreateFramebufferSurface(VkInstance instance, struct WindowSystem::WindowHandleInfo& windowInfo);

	void AppendOverlayDebugInfo() override;

	void ImguiInit();
	VkInstance GetVkInstance() const { return m_instance; }
	VkDevice GetLogicalDevice() const { return m_logicalDevice; }
	VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
	VkDescriptorPool GetDescriptorPool() const { return m_descriptorPool; }
	void WaitDeviceIdle() const { vkDeviceWaitIdle(m_logicalDevice); }

	void Initialize() override;
	void Shutdown() override;

	void SwapBuffers(bool swapTV = true, bool swapDRC = true) override;

	void Flush(bool waitIdle = false) override;
	void NotifyLatteCommandProcessorIdle() override;
	void SurfaceSync(Latte::E_COHER_CNTL coher, MPTR address, uint32 size) override;

	uint64 GenUniqueId(); // ids from a global counter, never zero

	void DrawEmptyFrame(bool mainWindow) override;

	// command buffer ring
	void InitFirstCommandBuffer();
	void ProcessFinishedCommandBuffers();
	void WaitForNextFinishedCommandBuffer();
	void SubmitCommandBuffer(VkSemaphore signalSemaphore = VK_NULL_HANDLE, VkSemaphore waitSemaphore = VK_NULL_HANDLE);
	void RequestSubmitSoon();
	void RequestSubmitOnIdle();

	// command buffer ids count submissions; an id is finished once its fence was observed signalled
	uint64 GetCurrentCommandBufferId() const;
	bool HasCommandBufferFinished(uint64 commandBufferId) const;
	void WaitCommandBufferFinished(uint64 commandBufferId);

	// objects still referenced by in-flight command buffers wait here until they can be deleted
	void ReleaseDestructibleObject(VKRDestructibleObject* destructibleObject);
	void ProcessDestructionQueue();

	FSpinlock m_spinlockDestructionQueue;
	std::vector<VKRDestructibleObject*> m_destructionQueue;

	void PipelineCacheSaveThread(size_t cache_size);

	void ClearColorbuffer(bool padView) override;
	void ClearColorImageRaw(VkImage image, uint32 sliceIndex, uint32 mipIndex, const VkClearColorValue& color, VkImageLayout inputLayout, VkImageLayout outputLayout);
	void ClearColorImage(LatteTextureVk* vkTexture, uint32 sliceIndex, uint32 mipIndex, const VkClearColorValue& color, VkImageLayout outputLayout);

	void DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter, sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground) override;
	void CreateDescriptorPool();
	VkDescriptorSet backbufferBlit_createDescriptorSet(VkDescriptorSetLayout descriptor_set_layout, LatteTextureViewVk* texViewVk, bool useLinearTexFilter);

	// minimal state hash -> full state hash -> pipeline
	robin_hood::unordered_flat_map<uint64, robin_hood::unordered_flat_map<uint64, PipelineInfo*> > m_pipeline_info_cache;
	void draw_debugPipelineHashState();
	PipelineInfo* draw_getCachedPipeline();

	static uint64 draw_calculateMinimalGraphicsPipelineHash(const LatteFetchShader* fetchShader, const LatteContextRegister& lcr);
	static uint64 draw_calculateGraphicsPipelineHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const VKRObjectRenderPass* renderPassObj, const LatteContextRegister& lcr);

	// render targets
	void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ = false) override;
	void renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight) override;

	LatteCachedFBO* rendertarget_createCachedFBO(uint64 key) override;
	void rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo) override;

	// textures
	void* texture_acquireTextureUploadBuffer(uint32 size) override;
	void texture_releaseTextureUploadBuffer(uint8* mem) override;

	TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) override;

	void texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex) override;
	void texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a) override;
	void texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue) override;

	void texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize) override;

	LatteTexture* texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth) override;

	void texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit) override;

	void texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth) override;
	LatteTextureReadbackInfo* texture_createReadback(LatteTextureView* textureView) override;

	// surface copy with format conversion (VulkanSurfaceCopy.cpp)
	void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height) override;
	void surfaceCopy_notifyTextureRelease(LatteTextureVk* hostTexture);

private:
	void surfaceCopy_viaDrawcall(LatteTextureVk* srcTextureVk, sint32 texSrcMip, sint32 texSrcSlice, LatteTextureVk* dstTextureVk, sint32 texDstMip, sint32 texDstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight);
	void surfaceCopy_cleanup();

	uint64 copySurface_getPipelineStateHash(struct VkCopySurfaceState_t& state);
	struct CopySurfacePipelineInfo* copySurface_getCachedPipeline(struct VkCopySurfaceState_t& state);
	struct CopySurfacePipelineInfo* copySurface_getOrCreateGraphicsPipeline(struct VkCopySurfaceState_t& state);
	VKRObjectTextureView* surfaceCopy_createImageView(LatteTextureVk* textureVk, uint32 sliceIndex, uint32 mipIndex);
	VKRObjectFramebuffer* surfaceCopy_getOrCreateFramebuffer(struct VkCopySurfaceState_t& state, struct CopySurfacePipelineInfo* pipelineInfo);
	VKRObjectDescriptorSet* surfaceCopy_getOrCreateDescriptorSet(struct VkCopySurfaceState_t& state, struct CopySurfacePipelineInfo* pipelineInfo);
	VKRObjectRenderPass* copySurface_createRenderpass(struct VkCopySurfaceState_t& state);

	std::unordered_map<uint64, struct CopySurfacePipelineInfo*> m_copySurfacePipelineCache;

public:
	// buffer cache, vertex and uniform bindings
	void bufferCache_init(const sint32 bufferSize) override;
	void bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset) override;
	void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) override;

	void buffer_bindVertexBuffers(std::span<BindBufferParam> bindings) override;
	void buffer_bindVertexStrideWorkaroundBuffer(VkBuffer fixedBuffer, uint32 offset, uint32 bufferIndex, uint32 size);
	std::pair<VkBuffer, uint32> buffer_genStrideWorkaroundVertexBuffer(MPTR buffer, uint32 size, uint32 oldStride);
	void buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size) override;

	RendererShader* shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackShader) override;

	IndexAllocation indexData_reserveIndexMemory(uint32 size) override;
	void indexData_releaseIndexMemory(IndexAllocation& allocation) override;
	void indexData_uploadIndexMemory(IndexAllocation& allocation) override;

	void GetTextureFormatInfoVK(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, sint32 width, sint32 height, FormatInfoVK* formatInfoOut);
	void unregisterGraphicsPipeline(PipelineInfo* pipelineInfo);

private:
	// state of the command buffer currently being recorded and of the emulated GPU bindings
	struct VkRendererState
	{
		VkRendererState() = default;
		VkRendererState(const VkRendererState&) = delete;
		VkRendererState(VkRendererState&&) noexcept = delete;

		LatteTextureViewVk* boundTexture[128]{};

		CachedFBOVk* activeFBO{}; // render target selected by the guest
		CachedFBOVk* activeRenderpassFBO{}; // render target of the Vulkan render pass that is open right now

		VkCommandBuffer currentCommandBuffer{};
		VkPipeline currentPipeline{ VK_NULL_HANDLE };

		PipelineInfo* activePipelineInfo{ nullptr };
		VkDescriptorSetInfo* activeVertexDS{ nullptr };
		VkDescriptorSetInfo* activePixelDS{ nullptr };
		VkDescriptorSetInfo* activeGeometryDS{ nullptr };
		bool descriptorSetsChanged{ false };
		CachedFBOVk::RendertargetSelfDependencyMask m_curRenderpassSelfDependencyInfo{};
		VkImageAspectFlags feedbackLoopImageAspect{ 0xFFFFFFFF };

		VkViewport currentViewport{};
		VkRect2D currentScissorRect{};

		struct
		{
			uint32 offset;
		}currentVertexBinding[Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS]{};

		Renderer::INDEX_TYPE activeIndexType{};
		uint32 activeIndexBufferIndex{};
		uint32 activeIndexBufferOffset{};

		// last depth bias values sent, as raw register bits
		uint32 prevPolygonFrontOffsetU32{ 0xFFFFFFFF };
		uint32 prevPolygonFrontScaleU32{ 0xFFFFFFFF };
		uint32 prevPolygonFrontClampU32{ 0xFFFFFFFF };

		// forgets everything that is tied to one command buffer, so the next draw sets it again
		void resetCommandBufferState()
		{
			prevPolygonFrontOffsetU32 = prevPolygonFrontScaleU32 = prevPolygonFrontClampU32 = 0xFFFFFFFF;
			currentPipeline = VK_NULL_HANDLE;
			for (auto& binding : currentVertexBinding)
				binding.offset = 0xFFFFFFFF;
			activeIndexType = Renderer::INDEX_TYPE::NONE;
			activeIndexBufferIndex = 0xFFFFFFFFu;
			activeIndexBufferOffset = 0xFFFFFFFFu;
			feedbackLoopImageAspect = 0xFFFFFFFF;
		}

		uint64 currentFlushIndex{ 0 };
		bool colorBufferSyncPending{ false }; // guest colour buffer sync seen since the previous draw; kept across command buffers
		bool requestFlush{ false }; // submit after every draw (render pass dependencies do not cover draws within one pass)

		bool drawSequenceSkip{ false }; // set by draw_beginSequence when the sequence cannot be drawn
	}m_state;

	std::unique_ptr<SwapchainInfoVk> m_mainSwapchainInfo{}, m_padSwapchainInfo{};
	std::atomic_flag m_destroyPadSwapchainNextAcquire{};
	bool IsSwapchainInfoValid(bool mainWindow) const;

	VkRenderPass m_imguiRenderPass = VK_NULL_HANDLE;

	VkDescriptorPool m_descriptorPool{ VK_NULL_HANDLE };

public:
	struct QueueFamilyIndices
	{
		int32_t graphicsFamily = -1;
		int32_t presentFamily = -1;

		bool IsComplete() const { return graphicsFamily >= 0 && presentFamily >= 0; }
	};
	static QueueFamilyIndices FindQueueFamilies(VkSurfaceKHR surface, VkPhysicalDevice device);

private:
	// what the device offers and what PixelU decided to use
	struct FeatureControl
	{
		struct
		{
			// a new optional extension also needs entries in CheckDeviceExtensionSupport and CreateDeviceCreateInfo
			bool tooling_info = false; // VK_EXT_tooling_info
			bool depth_range_unrestricted = false; // VK_EXT_depth_range_unrestricted
			bool nv_fill_rectangle = false; // VK_NV_fill_rectangle
			bool pipeline_feedback = false; // VK_EXT_pipeline_creation_feedback
			bool pipeline_creation_cache_control = false; // VK_EXT_pipeline_creation_cache_control
			bool custom_border_color = false; // VK_EXT_custom_border_color
			bool custom_border_color_without_format = false; // customBorderColorWithoutFormat feature of the above
			bool cubic_filter = false; // VK_EXT_filter_cubic
			bool driver_properties = false; // VK_KHR_driver_properties
			bool external_memory_host = false; // VK_EXT_external_memory_host
			bool synchronization2 = false; // VK_KHR_synchronization2
			bool dynamic_rendering = false; // VK_KHR_dynamic_rendering (never enabled)
			bool shader_float_controls = false; // VK_KHR_shader_float_controls
			bool present_wait = false; // VK_KHR_present_wait together with VK_KHR_present_id
			bool depth_clip_enable = false; // VK_EXT_depth_clip_enable
			bool pipeline_robustness = false; // VK_EXT_pipeline_robustness
			bool attachment_feedback_loop_layout = false; // VK_EXT_attachment_feedback_loop_layout
			bool attachment_feedback_loop_dynamic_state = false; // VK_EXT_attachment_feedback_loop_dynamic_state, only used together with the layout extension
		}deviceExtensions;

		struct
		{
			bool shaderRoundingModeRTEFloat32{ false };
		}shaderFloatControls;

		struct
		{
			bool debug_utils = false; // VK_EXT_debug_utils
		}instanceExtensions;

		struct
		{
			uint32 minUniformBufferOffsetAlignment = 256;
			uint32 nonCoherentAtomSize = 256;
			uint32 calcUniformBufferAlignmentM1{}; // max of the two above, minus one
		}limits;

		bool usingDebugMarkerTool{ false }; // a tool that understands object names is attached
		bool usingTracingTool{ false }; // a frame capture / replay tool is attached
		bool disableMultithreadedCompilation{ false }; // old NVIDIA drivers
	}m_featureControl{};
	static bool CheckDeviceExtensionSupport(const VkPhysicalDevice device, FeatureControl& info);
	static std::vector<const char*> CheckInstanceExtensionSupport(FeatureControl& info);

	bool UpdateSwapchainProperties(bool mainWindow);
	void SwapBuffer(bool mainWindow);

	VkDescriptorSetLayout m_swapchainDescriptorSetLayout{ VK_NULL_HANDLE };

	VkQueue m_graphicsQueue{ VK_NULL_HANDLE }, m_presentQueue{ VK_NULL_HANDLE };

	std::vector<VkDeviceQueueCreateInfo> CreateQueueCreateInfos(const std::set<int>& uniqueQueueFamilies) const;
	VkDeviceCreateInfo CreateDeviceCreateInfo(const std::vector<VkDeviceQueueCreateInfo>& queueCreateInfos, const VkPhysicalDeviceFeatures& deviceFeatures, const void* deviceExtensionStructs, std::vector<const char*>& used_extensions) const;
	static bool IsDeviceSuitable(VkSurfaceKHR surface, const VkPhysicalDevice& device);

	void CreateCommandPool();
	void CreateCommandBuffers();

	void swapchain_createDescriptorSetLayout();

	bool IsAsyncPipelineAllowed(uint32 numIndices);

	uint64 GetDescriptorSetStateHash(LatteDecompilerShader* shader);

	// imgui
	bool ImguiBegin(bool mainWindow) override;
	void ImguiEnd() override;
	ImTextureID GenerateTexture(const std::vector<uint8>& data, const Vector2i& size) override;
	void DeleteTexture(ImTextureID id) override;
	void DeleteFontTextures() override;
	bool BeginFrame(bool mainWindow) override;

	bool UseTFViaSSBO() const override { return true; }

	// draw calls (VulkanRendererCore.cpp)
	PipelineInfo* draw_createGraphicsPipeline(uint32 indexCount);
	PipelineInfo* draw_getOrCreateGraphicsPipeline(uint32 indexCount);

	void draw_updateVkBlendConstants();
	void draw_updateDepthBias(bool forceUpdate);

	void draw_setRenderPass();
	void draw_endRenderPass();

	void draw_beginSequence() override;
	void draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext) override;
	void draw_endSequence() override;

	void draw_updateVertexBuffersDirectAccess();
	void draw_updateUniformBuffersDirectAccess(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset, LatteConst::ShaderType shaderType);

	void draw_prepareDynamicOffsetsForDescriptorSet(uint32 shaderStageIndex, uint32* dynamicOffsets, sint32& numDynOffsets, const PipelineInfo* pipeline_info);
	VkDescriptorSetInfo* draw_getOrCreateDescriptorSet(PipelineInfo* pipeline_info, LatteDecompilerShader* shader);
	void draw_prepareDescriptorSets(PipelineInfo* pipeline_info, VkDescriptorSetInfo*& vertexDS, VkDescriptorSetInfo*& pixelDS, VkDescriptorSetInfo*& geometryDS);
	void draw_handleSpecialState5();

	void sync_inputTexturesChanged(bool withinFeedbackLoopRenderPass = false);
	void sync_RenderPassLoadTextures(CachedFBOVk* fboVk);
	void sync_RenderPassStoreTextures(CachedFBOVk* fboVk);

	VkCommandBuffer getCurrentCommandBuffer() const { return m_state.currentCommandBuffer; }

	// uniforms
	uint32 uniformData_uploadUniformDataBufferGetOffset(std::span<uint8, std::dynamic_extent> data);
	void uniformData_updateUniformVars(uint32 shaderStageIndex, LatteDecompilerShader* shader, float* __restrict uniformBuf);
	void uniformData_updateUniformVarsIncremental(uint32 shaderStageIndex, LatteDecompilerShader* shader, uint8& stageUniformModifiedMask, float* __restrict uniformBuf, bool aluConstDirty, uint32 uniformBufferDirtyMask);

	void CreatePipelineCache();
	VkPipelineShaderStageCreateInfo CreatePipelineShaderStageCreateInfo(VkShaderStageFlagBits stage, VkShaderModule& module, const char* entryName) const;
	VkPipeline backbufferBlit_createGraphicsPipeline(VkDescriptorSetLayout descriptorLayout, bool padView, RendererOutputShader* shader);
	bool AcquireNextSwapchainImage(bool mainWindow);
	void RecreateSwapchain(bool mainWindow, bool skipCreate = false);

	// streamout
	void streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize) override;
	void streamout_begin() override;
	void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void streamout_rendererFinishDrawcall() override;

	// occlusion queries (VulkanQuery.cpp)
	LatteQueryObject* occlusionQuery_create() override;
	void occlusionQuery_destroy(LatteQueryObject* queryObj) override;
	void occlusionQuery_flush() override;
	void occlusionQuery_updateState() override;
	void occlusionQuery_notifyEndCommandBuffer();
	void occlusionQuery_notifyBeginCommandBuffer();

private:
	std::vector<const char*> m_layerNames;
	VkInstance m_instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
	VkDevice m_logicalDevice = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT m_debugCallback = nullptr;
	volatile bool m_destructionRequested = false;

	QueueFamilyIndices m_indices{};

	// driver pipeline cache and the thread that writes it to disk
	Semaphore m_pipeline_cache_semaphore;
	std::shared_mutex m_pipeline_cache_save_mutex;
	std::thread m_pipeline_cache_save_thread;
	VkPipelineCache m_pipeline_cache{ nullptr };

	// presentation blits
	std::unordered_map<uint64, VkPipeline> m_backbufferBlitPipelineCache;
	std::unordered_map<uint64, VkDescriptorSet> m_backbufferBlitDescriptorSetCache;
	VkPipelineLayout m_pipelineLayout{ nullptr };
	VkCommandPool m_commandPool{ nullptr };

	// ring buffer for uniform data written by the CPU
	VkBuffer m_uniformVarBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_uniformVarBufferMemory = VK_NULL_HANDLE;
	bool m_uniformVarBufferMemoryIsCoherent{ false };
	uint8* m_uniformVarBufferPtr = nullptr;
	uint32 m_uniformVarBufferWriteIndex = 0;
	uint32 m_uniformVarBufferReadIndex = 0;

	// streamout target ring
	VkBuffer m_xfbRingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_xfbRingBufferMemory = VK_NULL_HANDLE;

	// GPU copy of guest vertex, uniform and streamout data
	VkBuffer m_bufferCache = VK_NULL_HANDLE;
	VkDeviceMemory m_bufferCacheMemory = VK_NULL_HANDLE;

	// host visible staging for texture readbacks
	VkBuffer m_textureReadbackBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_textureReadbackBufferMemory = VK_NULL_HANDLE;
	uint8* m_textureReadbackBufferPtr = nullptr;
	uint32 m_textureReadbackBufferWriteIndex = 0;

	// bound in place of missing textures
	struct NullTexture
	{
		VkImage image;
		VkImageView view;
		VkSampler sampler;
		VkImageMemAllocation* allocation;
	};

	NullTexture nullTexture1D{};
	NullTexture nullTexture2D{};

	void CreateNullTexture(NullTexture& nullTex, VkImageType imageType);
	void CreateNullObjects();
	void DeleteNullTexture(NullTexture& nullTex);
	void DeleteNullObjects();

	// Importing guest RAM as a Vulkan buffer (VK_EXT_external_memory_host) is not used; the constant keeps the
	// related branches compiled out.
	static constexpr bool m_useHostMemoryForCache{ false };
	VkBuffer m_importedMem = VK_NULL_HANDLE;
	VkDeviceMemory m_importedMemMemory = VK_NULL_HANDLE;
	MPTR m_importedMemBaseAddress = 0;

	// command buffer ring
	static constexpr uint32 kCommandBufferPoolSize = 128;

	size_t m_commandBufferIndex = 0; // buffer being recorded
	size_t m_commandBufferSyncIndex = 0; // oldest submitted buffer not known to be finished
	size_t m_commandBufferIDOfPrevFrame = 0;
	std::array<size_t, kCommandBufferPoolSize> m_cmdBufferUniformRingbufIndices{}; // uniform ring read position once the buffer finished
	std::array<VkFence, kCommandBufferPoolSize> m_cmdBufferFences{};
	std::array<VkCommandBuffer, kCommandBufferPoolSize> m_commandBuffers{};
	std::array<VkSemaphore, kCommandBufferPoolSize> m_commandBufferSemaphores{};

	VkSemaphore GetLastSubmittedCmdBufferSemaphore()
	{
		const size_t n = m_commandBufferSemaphores.size();
		return m_commandBufferSemaphores[(m_commandBufferIndex + n - 1) % n];
	}

	uint64 m_numSubmittedCmdBuffers{};
	uint64 m_countCommandBufferFinished{};

	uint32 m_recordedDrawcalls{}; // draws recorded into the current command buffer
	uint32 m_submitThreshold{}; // submit once m_recordedDrawcalls passes this
	bool m_submitOnIdle{}; // submit when the command processor runs out of work

	void draw_execute_first(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext);
	void draw_execute_continued(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext);
	// steps shared by draw_execute_first and draw_execute_continued
	bool DrawHandleSpecialStates();
	void DrawPrepareIndices(MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, uint32 count, uint32& indexMax, Renderer::INDEX_TYPE& hostIndexType, uint32& hostIndexCount);
	void DrawUpdateDirectAccessBuffers();
	void DrawBindPipeline(PipelineInfo* pipelineInfo);
	void DrawBindDescriptorSet(PipelineInfo* pipelineInfo, uint32 firstSet, VkDescriptorSetInfo* first, uint32 firstStage, VkDescriptorSetInfo* second, uint32 secondStage);
	void DrawIssue(Renderer::INDEX_TYPE hostIndexType, uint32 hostIndexCount, uint32 count, uint32 instanceCount, uint32 baseVertex, uint32 baseInstance);

	// dynamic offsets of the uniform ring and of the guest uniform buffers, per stage
	struct
	{
		uint32 uniformVarBufferOffset[VulkanRendererConst::SHADER_STAGE_INDEX_COUNT];
		struct
		{
			uint32 uniformBufferOffset[LATTE_NUM_MAX_UNIFORM_BUFFERS];
		}shaderUB[VulkanRendererConst::SHADER_STAGE_INDEX_COUNT];
	}dynamicOffsetInfo{};

	struct
	{
		struct
		{
			bool enabled;
			uint32 ringBufferOffset;
		}buffer[LATTE_NUM_STREAMOUT_BUFFER];
		sint32 verticesPerInstance;
	}m_streamoutState{};

	struct
	{
		VkQueryPool queryPool{ VK_NULL_HANDLE };
		sint32 currentQueryIndex{};
		std::vector<class LatteQueryObjectVk*> list_cachedQueries;
		std::vector<class LatteQueryObjectVk*> list_currentlyActiveQueries;
		uint64 m_lastCommandBuffer{};
		VkBuffer bufferQueryResults{ VK_NULL_HANDLE };
		VkDeviceMemory memoryQueryResults{ VK_NULL_HANDLE };
		uint64* ptrQueryResults{ nullptr };
		std::vector<uint16> list_availableQueryIndices;
	}m_occlusionQueries;

	// Barriers. Each SYNC_OP bit names a kind of access; barrier helpers translate a set of bits into the pipeline
	// stages and access masks that cover it, for the source and the destination side separately.
	enum SYNC_OP : uint32
	{
		HOST_WRITE = 0x01,
		HOST_READ = 0x02,
		BUFFER_SHADER_READ = 0x04, // vertex fetch, uniform and storage reads from shaders
		BUFFER_SHADER_WRITE = 0x08, // storage writes from shaders
		ANY_TRANSFER = 0x10, // copy commands, reading or writing
		IMAGE_READ = 0x20, // sampling and attachment reads
		IMAGE_WRITE = 0x40, // attachment writes
		TRANSFER_READ = 0x80,
		TRANSFER_WRITE = 0x100,
	};

	struct SyncScope
	{
		VkPipelineStageFlags stages;
		VkAccessFlags access;
	};

	static constexpr SyncScope SyncScopeOf(uint32 ops)
	{
		constexpr VkPipelineStageFlags shaderStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		constexpr VkPipelineStageFlags fragmentTests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		SyncScope s{ 0, 0 };
		if (ops & BUFFER_SHADER_READ)
		{
			// index reads are synchronised separately
			s.stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | shaderStages;
			s.access |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
		}
		if (ops & BUFFER_SHADER_WRITE)
		{
			s.stages |= shaderStages;
			s.access |= VK_ACCESS_SHADER_WRITE_BIT;
		}
		if (ops & ANY_TRANSFER)
		{
			s.stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
			s.access |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		}
		if (ops & TRANSFER_READ)
		{
			s.stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
			s.access |= VK_ACCESS_TRANSFER_READ_BIT;
		}
		if (ops & TRANSFER_WRITE)
		{
			s.stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
			s.access |= VK_ACCESS_TRANSFER_WRITE_BIT;
		}
		if (ops & HOST_WRITE)
		{
			s.stages |= VK_PIPELINE_STAGE_HOST_BIT;
			s.access |= VK_ACCESS_HOST_WRITE_BIT;
		}
		if (ops & HOST_READ)
		{
			s.stages |= VK_PIPELINE_STAGE_HOST_BIT;
			s.access |= VK_ACCESS_HOST_READ_BIT;
		}
		if (ops & IMAGE_READ)
		{
			s.stages |= shaderStages | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragmentTests;
			s.access |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		}
		if (ops & IMAGE_WRITE)
		{
			s.stages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragmentTests;
			s.access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		}
		return s;
	}

	template<uint32 TSyncOp>
	void barrier_calcStageAndMask(VkPipelineStageFlags& stages, VkAccessFlags& accessFlags)
	{
		constexpr SyncScope s = SyncScopeOf(TSyncOp);
		stages = s.stages;
		accessFlags = s.access;
	}

	static VkBufferMemoryBarrier MakeBufferBarrier(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkAccessFlags srcAccess, VkAccessFlags dstAccess)
	{
		VkBufferMemoryBarrier b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		b.srcAccessMask = srcAccess;
		b.dstAccessMask = dstAccess;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.buffer = buffer;
		b.offset = offset;
		b.size = size;
		return b;
	}

	template<uint32 TSrcSyncOp, uint32 TDstSyncOp>
	void barrier_bufferRange(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
	{
		constexpr SyncScope src = SyncScopeOf(TSrcSyncOp);
		constexpr SyncScope dst = SyncScopeOf(TDstSyncOp);
		const VkBufferMemoryBarrier b = MakeBufferBarrier(buffer, offset, size, src.access, dst.access);
		vkCmdPipelineBarrier(m_state.currentCommandBuffer, src.stages, dst.stages, 0, 0, nullptr, 1, &b, 0, nullptr);
	}

	// two buffer ranges with independent scopes in a single barrier command
	template<uint32 TSrcSyncOpA, uint32 TDstSyncOpA, uint32 TSrcSyncOpB, uint32 TDstSyncOpB>
	void barrier_bufferRange(VkBuffer bufferA, VkDeviceSize offsetA, VkDeviceSize sizeA,
							 VkBuffer bufferB, VkDeviceSize offsetB, VkDeviceSize sizeB)
	{
		constexpr SyncScope srcA = SyncScopeOf(TSrcSyncOpA), dstA = SyncScopeOf(TDstSyncOpA);
		constexpr SyncScope srcB = SyncScopeOf(TSrcSyncOpB), dstB = SyncScopeOf(TDstSyncOpB);
		const VkBufferMemoryBarrier b[2] = {
			MakeBufferBarrier(bufferA, offsetA, sizeA, srcA.access, dstA.access),
			MakeBufferBarrier(bufferB, offsetB, sizeB, srcB.access, dstB.access) };
		vkCmdPipelineBarrier(m_state.currentCommandBuffer, srcA.stages | srcB.stages, dstA.stages | dstB.stages, 0, 0, nullptr, 2, b, 0, nullptr);
	}

	// orders every transfer before all later commands
	void barrier_sequentializeTransfer()
	{
		VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		vkCmdPipelineBarrier(m_state.currentCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
	}

	// execution dependency only: everything before finishes before anything after starts
	void barrier_sequentializeCommand()
	{
		vkCmdPipelineBarrier(m_state.currentCommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
	}

	template<uint32 TSrcSyncOp, uint32 TDstSyncOp>
	void barrier_image(VkImage imageVk, const VkImageSubresourceRange& subresourceRange, VkImageLayout oldLayout, VkImageLayout newLayout)
	{
		constexpr SyncScope src = SyncScopeOf(TSrcSyncOp);
		constexpr SyncScope dst = SyncScopeOf(TDstSyncOp);
		VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.srcAccessMask = src.access;
		b.dstAccessMask = dst.access;
		b.oldLayout = oldLayout;
		b.newLayout = newLayout;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = imageVk;
		b.subresourceRange = subresourceRange;
		vkCmdPipelineBarrier(m_state.currentCommandBuffer, src.stages, dst.stages, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	// transitions one mip level of a texture's slices and records the new layout in the texture's layout tracking
	template<uint32 TSrcSyncOp, uint32 TDstSyncOp>
	void barrier_image(LatteTextureVk* vkTexture, const VkImageSubresourceLayers& subresourceLayers, VkImageLayout newLayout)
	{
		VkImageSubresourceRange range;
		range.aspectMask = subresourceLayers.aspectMask;
		range.baseMipLevel = subresourceLayers.mipLevel;
		range.levelCount = 1;
		range.baseArrayLayer = subresourceLayers.baseArrayLayer;
		range.layerCount = subresourceLayers.layerCount;
		barrier_image<TSrcSyncOp, TDstSyncOp>(vkTexture->GetImageObj()->m_image, range, vkTexture->GetImageLayout(range), newLayout);
		vkTexture->SetImageLayout(range, newLayout);
	}

public:
	bool GetDisableMultithreadedCompilation() const { return m_featureControl.disableMultithreadedCompilation; }
	bool HasSPRIVRoundingModeRTE32() const { return m_featureControl.shaderFloatControls.shaderRoundingModeRTEFloat32; }
	bool IsDebugMarkersEnabled() const { return m_featureControl.usingDebugMarkerTool; }
	bool IsTracingToolEnabled() const { return m_featureControl.usingTracingTool; }
	bool UseAttachmentFeedbackLoop() const { return m_featureControl.deviceExtensions.attachment_feedback_loop_dynamic_state; }

private:
	void debug_genericBarrier();

	// built-in shaders for surface copies between colour and depth formats
	struct
	{
		RendererShaderVk* copySurface_vs{};
		RendererShaderVk* copySurface_psDepth2Color{};
		RendererShaderVk* copySurface_psColor2Depth{};
	}defaultShaders;
};
