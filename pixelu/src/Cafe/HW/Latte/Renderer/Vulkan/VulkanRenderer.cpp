// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Vulkan renderer: instance and device bring-up, command buffer ring, swapchain presentation,
// texture and buffer-cache operations, plus the small VKR* object wrappers.

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanTextureReadback.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/CocoaSurface.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"

#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteTiming.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/GameInfo.h"

#include "util/helpers/helpers.h"
#include "util/helpers/StringHelpers.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "WindowSystem.h"

#include "imgui/imgui_extension.h"
#include "imgui/imgui_impl_vulkan.h"

#include <cstdint>
#include <glslang/Public/ShaderLang.h>

#ifndef VK_API_VERSION_MAJOR
#define VK_API_VERSION_MAJOR(v) (((uint32_t)(v) >> 22) & 0x7FU)
#define VK_API_VERSION_MINOR(v) (((uint32_t)(v) >> 12) & 0x3FFU)
#endif

extern std::atomic_int g_compiling_pipelines;

namespace
{
	// device extensions without which the renderer cannot run
	const char* const s_mandatoryDeviceExtensions[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME,
	};

	bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
	{
		for (const VkExtensionProperties& e : list)
		{
			if (strcmp(e.extensionName, name) == 0)
				return true;
		}
		return false;
	}

	// instance API version to request: 1.1 unless the loader reports something
	uint32 SelectInstanceApiVersion()
	{
		uint32 version = VK_API_VERSION_1_1;
		if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS)
		{
			const bool belowOnePointTwo = VK_API_VERSION_MAJOR(version) < 1 || VK_API_VERSION_MINOR(version) < 2;
			if (belowOnePointTwo)
				version = VK_API_VERSION_1_1;
		}
		return version;
	}

	VkApplicationInfo MakeApplicationInfo(uint32 apiVersion)
	{
		VkApplicationInfo info{};
		info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		info.pApplicationName = EMULATOR_NAME;
		info.pEngineName = EMULATOR_NAME;
		info.applicationVersion = VK_MAKE_VERSION(EMULATOR_VERSION_MAJOR, EMULATOR_VERSION_MINOR, EMULATOR_VERSION_PATCH);
		info.engineVersion = info.applicationVersion;
		info.apiVersion = apiVersion;
		return info;
	}

	// the window-system surface extensions this build presents through
	void AddSurfaceExtensions(std::vector<const char*>& out)
	{
		out.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if BOOST_OS_WINDOWS
		out.emplace_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif BOOST_OS_LINUX || BOOST_OS_BSD
		const auto backend = WindowSystem::GetWindowInfo().window_main.backend;
		if (backend == WindowSystem::WindowHandleInfo::Backend::X11)
			out.emplace_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#ifdef HAS_WAYLAND
		else if (backend == WindowSystem::WindowHandleInfo::Backend::Wayland)
			out.emplace_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif
#elif BOOST_OS_MACOS
		out.emplace_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#endif
	}

	// builds a pNext chain front to back: every Link() puts the struct in front of the ones linked before
	class StructChain
	{
	public:
		template<typename T>
		T& Link(T& s, VkStructureType type)
		{
			s.sType = type;
			s.pNext = m_head;
			m_head = &s;
			return s;
		}
		void* Head() const { return m_head; }
	private:
		void* m_head = nullptr;
	};

	void* MapWholeMemory(VkDevice device, VkDeviceMemory memory)
	{
		void* ptr = nullptr;
		vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &ptr);
		return ptr;
	}

	VkPhysicalDeviceProperties2 QueryDeviceIdProperties(VkPhysicalDevice device, VkPhysicalDeviceIDProperties& idProps)
	{
		idProps = {};
		idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
		VkPhysicalDeviceProperties2 props{};
		props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		props.pNext = &idProps;
		vkGetPhysicalDeviceProperties2(device, &props);
		return props;
	}
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
#ifdef CEMU_DEBUG_ASSERT
	// messages that are known and accepted (feedback loops, swizzled attachments, sampler counts, SPIR-V quirks)
	static const char* const ignoredText[] = {
		"consumes input location",
		"blend",
		"Number of currently valid sampler objects is not less than the maximum allowed",
	};
	static const sint32 ignoredIds[] = {
		(sint32)0x6c3b517c, (sint32)0xa6b17cdf, (sint32)0xc406fcb7,
		(sint32)0x6e633069, (sint32)0xb408bc0b, (sint32)0x006bbb14,
	};
	for (const char* text : ignoredText)
	{
		if (strstr(pCallbackData->pMessage, text))
			return VK_FALSE;
	}
	for (sint32 id : ignoredIds)
	{
		if (pCallbackData->messageIdNumber == id)
			return VK_FALSE;
	}
#endif
	cemuLog_log(LogType::Force, (char*)pCallbackData->pMessage);
	return VK_FALSE;
}

std::vector<VulkanRenderer::DeviceInfo> VulkanRenderer::GetDevices()
{
	if (!vkEnumerateInstanceVersion)
	{
		cemuLog_log(LogType::Force, "Vulkan cant list devices because Vulkan loader failed");
		return {};
	}
	const VkApplicationInfo appInfo = MakeApplicationInfo(SelectInstanceApiVersion());

	std::vector<const char*> extensions;
	AddSurfaceExtensions(extensions);

	VkInstanceCreateInfo instanceInfo{};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;
	instanceInfo.enabledExtensionCount = (uint32)extensions.size();
	instanceInfo.ppEnabledExtensionNames = extensions.data();

	std::vector<DeviceInfo> found;
	VkInstance instance = nullptr;
	try
	{
		const VkResult r = vkCreateInstance(&instanceInfo, nullptr, &instance);
		if (r != VK_SUCCESS)
			throw std::runtime_error(fmt::format("Unable to create a Vulkan instance: {}", r));
		if (!InitializeInstanceVulkan(instance))
			throw std::runtime_error("can't initialize instanced vulkan functions");

		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		if (count == 0)
			throw std::runtime_error("Failed to find a GPU with Vulkan support.");

		// a throwaway surface lets IsDeviceSuitable check presentation support
		VkSurfaceKHR probeSurface = CreateFramebufferSurface(instance, WindowSystem::GetWindowInfo().window_main);
		std::vector<VkPhysicalDevice> physicalDevices(count);
		vkEnumeratePhysicalDevices(instance, &count, physicalDevices.data());
		for (VkPhysicalDevice pd : physicalDevices)
		{
			if (!IsDeviceSuitable(probeSurface, pd))
				continue;
			VkPhysicalDeviceIDProperties idProps;
			const VkPhysicalDeviceProperties2 props = QueryDeviceIdProperties(pd, idProps);
			found.emplace_back(props.properties.deviceName, idProps.deviceUUID);
		}
		vkDestroySurfaceKHR(instance, probeSurface, nullptr);
	}
	catch (...)
	{
	}
	if (instance)
		vkDestroyInstance(instance, nullptr);
	return found;
}

void VulkanRenderer::DetermineVendor()
{
	const bool haveDriverProps = m_featureControl.deviceExtensions.driver_properties;
	VkPhysicalDeviceDriverProperties driverProps{};
	driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
	VkPhysicalDeviceProperties2 props{};
	props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props.pNext = haveDriverProps ? &driverProps : nullptr;
	vkGetPhysicalDeviceProperties2(m_physicalDevice, &props);

	switch (props.properties.vendorID)
	{
	case 0x10DE: m_vendor = GfxVendor::Nvidia; break;
	case 0x8086: m_vendor = GfxVendor::Intel; break;
	case 0x1002: m_vendor = GfxVendor::AMD; break;
	case 0x106B: m_vendor = GfxVendor::Apple; break;
	default: break;
	}
	// open source Mesa drivers are treated as their own vendor class
	if (driverProps.driverID == VK_DRIVER_ID_MESA_RADV || driverProps.driverID == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA)
		m_vendor = GfxVendor::Mesa;

	cemuLog_log(LogType::Force, "Using GPU: {}", props.properties.deviceName);
	if (haveDriverProps)
	{
		cemuLog_log(LogType::Force, "Driver version: {}", driverProps.driverInfo);
		// Nvidia drivers before 515 do not handle pipeline creation from several threads well
		if (m_vendor == GfxVendor::Nvidia)
			m_featureControl.disableMultithreadedCompilation = StringHelpers::ToInt(std::string(driverProps.driverInfo)) < 515;
	}
	else
	{
		cemuLog_log(LogType::Force, "Driver version (as stored in device info): {:08}", props.properties.driverVersion);
		// without driver properties the driver is assumed to predate 515
		if (m_vendor == GfxVendor::Nvidia)
			m_featureControl.disableMultithreadedCompilation = true;
	}
}

void VulkanRenderer::GetDeviceFeatures()
{
	auto& ext = m_featureControl.deviceExtensions;

	// feature query
	VkPhysicalDeviceCustomBorderColorFeaturesEXT borderColor{};
	VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT cacheControl{};
	VkPhysicalDevicePresentIdFeaturesKHR presentId{};
	VkPhysicalDevicePresentWaitFeaturesKHR presentWait{};
	VkPhysicalDevicePipelineRobustnessFeaturesEXT robustness{};
	VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT feedbackDynState{};
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedbackLayout{};
	StructChain featureChain;
	featureChain.Link(borderColor, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
	featureChain.Link(cacheControl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT);
	featureChain.Link(presentId, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
	featureChain.Link(presentWait, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
	if (ext.pipeline_robustness)
		featureChain.Link(robustness, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT);
	if (ext.attachment_feedback_loop_dynamic_state)
		featureChain.Link(feedbackDynState, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT);
	if (ext.attachment_feedback_loop_layout)
		featureChain.Link(feedbackLayout, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT);
	VkPhysicalDeviceFeatures2 features2{};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = featureChain.Head();
	vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

	cemuLog_log(LogType::Force, "Vulkan: present_wait extension: {}", (presentWait.presentWait && presentId.presentId) ? "supported" : "unsupported");

	// property query
	VkPhysicalDeviceFloatControlsPropertiesKHR floatControls{};
	StructChain propChain;
	if (ext.shader_float_controls)
		propChain.Link(floatControls, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR);
	VkPhysicalDeviceProperties2 props2{};
	props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props2.pNext = propChain.Head();
	vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

	// derive what can actually be used
	ext.pipeline_creation_cache_control = cacheControl.pipelineCreationCacheControl;
	ext.custom_border_color_without_format = ext.custom_border_color && borderColor.customBorderColorWithoutFormat;
	m_featureControl.shaderFloatControls.shaderRoundingModeRTEFloat32 = ext.shader_float_controls && floatControls.shaderRoundingModeRTEFloat32;
	if (!m_featureControl.shaderFloatControls.shaderRoundingModeRTEFloat32)
		cemuLog_log(LogType::Force, "Shader round mode control not available on this device or driver. Some rendering issues might occur.");

	if (!ext.pipeline_creation_cache_control)
	{
		cemuLog_log(LogType::Force, "VK_EXT_pipeline_creation_cache_control not supported. Cannot use asynchronous shader and pipeline compilation");
		if (GetConfig().async_compile)
			LatteOverlay_pushNotification(_tr("Async shader compile is enabled but not supported by the graphics driver\nCemu will use synchronous compilation which can cause additional stutter"), 10000);
	}
	if (!ext.custom_border_color_without_format)
	{
		if (ext.custom_border_color)
			cemuLog_log(LogType::Force, "VK_EXT_custom_border_color is present but only with limited support. Cannot emulate arbitrary border color");
		else
			cemuLog_log(LogType::Force, "VK_EXT_custom_border_color not supported. Cannot emulate arbitrary border color");
	}
	if (!ext.depth_clip_enable)
		cemuLog_log(LogType::Force, "VK_EXT_depth_clip_enable not supported");
	if (ext.pipeline_robustness && robustness.pipelineRobustness != VK_TRUE)
		ext.pipeline_robustness = false;
	if (ext.attachment_feedback_loop_layout)
		ext.attachment_feedback_loop_layout = feedbackLayout.attachmentFeedbackLoopLayout == VK_TRUE;
	if (ext.attachment_feedback_loop_dynamic_state && ext.attachment_feedback_loop_layout)
		ext.attachment_feedback_loop_dynamic_state = feedbackDynState.attachmentFeedbackLoopDynamicState == VK_TRUE;
	if (!UseAttachmentFeedbackLoop())
		cemuLog_log(LogType::Force, "VK_EXT_attachment_feedback_loop_layout(_dynamic_state) not supported");

	// limits, never below 4 bytes
	const VkPhysicalDeviceLimits& limits = props2.properties.limits;
	auto& outLimits = m_featureControl.limits;
	outLimits.minUniformBufferOffsetAlignment = std::max(limits.minUniformBufferOffsetAlignment, (VkDeviceSize)4);
	outLimits.nonCoherentAtomSize = std::max(limits.nonCoherentAtomSize, (VkDeviceSize)4);
	cemuLog_log(LogType::Force, fmt::format("VulkanLimits: UBAlignment {0} nonCoherentAtomSize {1}", limits.minUniformBufferOffsetAlignment, limits.nonCoherentAtomSize));
	outLimits.calcUniformBufferAlignmentM1 = std::max(outLimits.minUniformBufferOffsetAlignment, outLimits.nonCoherentAtomSize) - 1;
}

#if BOOST_OS_LINUX
#include <sys/wait.h>
#include "resource/IconsFontAwesome5.h"

// Runs in a forked copy of the emulator (CEMU_DETECT_RADV set): enumerating devices with
// RADV_DEBUG=llvm aborts when Mesa was built without LLVM. Exit code 0 means LLVM is usable.
int BreathOfTheWildChildProcessMain()
{
	InitializeGlobalVulkan();
	struct sigaction onAbort{};
	onAbort.sa_handler = [](int) { _exit(1); };
	sigaction(SIGABRT, &onAbort, nullptr);
	freopen("/dev/null", "w", stderr);
	setenv("RADV_DEBUG", "llvm", 1);

	VkInstanceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
		return 1;
	InitializeInstanceVulkan(instance);
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	vkDestroyInstance(instance, nullptr);
	return 0;
}

namespace
{
	// driver version of the first AMD GPU driven by RADV, 0 when there is none
	uint32 FindRadvDriverVersion(VkInstance instance)
	{
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(instance, &count, devices.data());
		for (VkPhysicalDevice pd : devices)
		{
			VkPhysicalDeviceDriverProperties driver{};
			driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
			VkPhysicalDeviceProperties2 props{};
			props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			props.pNext = &driver;
			vkGetPhysicalDeviceProperties2(pd, &props);
			if (props.properties.vendorID == 0x1002 && driver.driverID == VK_DRIVER_ID_MESA_RADV)
				return props.properties.driverVersion;
		}
		return 0;
	}

	// Mesa 25.3.0 up to and including 26.0.4 crash in BOTW with the ACO compiler
	bool IsAffectedRadvVersion(uint32 version)
	{
		const int major = VK_API_VERSION_MAJOR(version);
		const int minor = VK_API_VERSION_MINOR(version);
		const int patch = VK_API_VERSION_PATCH(version);
		if (major > 26)
			return false;
		if (major == 26)
			return minor == 0 && patch < 5;
		// major 25 or below: affected unless the version is below x.3
		return !(minor < 3);
	}

	// Breath of the Wild triggers GPU hangs on some RADV versions unless the LLVM backend is used.
	// Switches RADV to LLVM (when Mesa has it) and recreates the instance so the setting applies.
	void LinuxBreathOfTheWildWorkaround(VkInstance& instance, const VkInstanceCreateInfo* createInfo)
	{
		const char* radvDebug = getenv("RADV_DEBUG");
		const std::string_view current = radvDebug ? radvDebug : "";
		// the user already picked a shader backend
		if (current.find("aco") != std::string_view::npos || current.find("llvm") != std::string_view::npos)
			return;

		const uint32 version = FindRadvDriverVersion(instance);
		if (version == 0 || !IsAffectedRadvVersion(version))
			return;

		// probe in a child process whether forcing LLVM works at all
		const int child = fork();
		if (child == 0)
		{
			setenv("CEMU_DETECT_RADV", "1", 1);
			execl("/proc/self/exe", "/proc/self/exe", nullptr);
			_exit(2);
		}
		int status = 0;
		waitpid(child, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) == 2)
		{
			cemuLog_log(LogType::Force, "BOTW/RADV workaround not applied because detecting LLVM presence failed unexpectedly");
			return;
		}
		if (WEXITSTATUS(status) == 1)
			cemuLog_log(LogType::Force, "BOTW/RADV workaround not applied because mesa was built without LLVM");
		if (WEXITSTATUS(status) != 0)
			return;

		cemuLog_log(LogType::Force, "BOTW/RADV workaround active. Adding \"llvm\" to RADV_DEBUG environment variable");
		std::string newValue(current);
		newValue.append(newValue.empty() ? "llvm" : ",llvm");
		setenv("RADV_DEBUG", newValue.c_str(), 1);

		vkDestroyInstance(instance, nullptr);
		const VkResult r = vkCreateInstance(createInfo, nullptr, &instance);
		if (r != VK_SUCCESS)
			throw std::runtime_error(fmt::format("Unable to re-create a Vulkan instance after RADV/LLVM workaround: {}", r));
		InitializeInstanceVulkan(instance);

		LatteOverlay_pushNotification(std::string{(const char*)ICON_FA_EXCLAMATION_TRIANGLE} + "RADV_DEBUG=llvm set automatically to avoid crashing due to a driver bug. If possible update mesa to 26.0.5 or newer", 10'000);
	}
}
#endif

VulkanRenderer::VulkanRenderer() : Renderer(RendererAPI::Vulkan)
{
	glslang::InitializeProcess();
	cemuLog_log(LogType::Force, "------- Init Vulkan graphics backend -------");

	const bool validation = cemuLog_isLoggingEnabled(LogType::VulkanValidation);
	if (validation)
		cemuLog_log(LogType::Force, "Validation layer is enabled");
	m_layerNames.clear();
	if (validation)
		m_layerNames.emplace_back("VK_LAYER_KHRONOS_validation");

	// ---- instance ----
	std::vector<const char*> instanceExtensions = CheckInstanceExtensionSupport(m_featureControl);
	const uint32 apiVersion = SelectInstanceApiVersion();
	cemuLog_log(LogType::Force, fmt::format("Vulkan instance version: {}.{}", VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion)));
	const VkApplicationInfo appInfo = MakeApplicationInfo(apiVersion);

	VkInstanceCreateInfo instanceInfo{};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;
	instanceInfo.enabledExtensionCount = (uint32)instanceExtensions.size();
	instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
	instanceInfo.enabledLayerCount = (uint32)m_layerNames.size();
	instanceInfo.ppEnabledLayerNames = m_layerNames.data();

	VkResult r = vkCreateInstance(&instanceInfo, nullptr, &m_instance);
	if (r == VK_ERROR_LAYER_NOT_PRESENT)
	{
		cemuLog_log(LogType::Force, "Failed to enable vulkan validation (VK_LAYER_KHRONOS_validation)");
		instanceInfo.enabledLayerCount = 0;
		r = vkCreateInstance(&instanceInfo, nullptr, &m_instance);
	}
	if (r != VK_SUCCESS)
		throw std::runtime_error(fmt::format("Unable to create a Vulkan instance: {}", r));
	if (!InitializeInstanceVulkan(m_instance))
		throw std::runtime_error("Unable to load instanced Vulkan functions");

#if BOOST_OS_LINUX
	// Breath of the Wild (EU, US, JP): runes like Magnesis hang the GPU on affected RADV versions
	switch (CafeSystem::GetForegroundTitleId())
	{
	case 0x00050000101c9500:
	case 0x00050000101c9400:
	case 0x00050000101c9300:
		LinuxBreathOfTheWildWorkaround(m_instance, &instanceInfo);
		break;
	default:
		break;
	}
#endif

	// ---- physical device ----
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
	if (deviceCount == 0)
		throw std::runtime_error("Failed to find a GPU with Vulkan support.");

	// temporary surface used for the suitability and queue family checks
	VkSurfaceKHR probeSurface = CreateFramebufferSurface(m_instance, WindowSystem::GetWindowInfo().window_main);

	auto& config = GetConfig();
	const decltype(config.vk_graphic_device_uuid) noDevice{};
	const bool userPickedDevice = config.vk_graphic_device_uuid != noDevice;

	VkPhysicalDevice firstSuitable = VK_NULL_HANDLE;
	std::string firstSuitableName;
	std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
	vkEnumeratePhysicalDevices(m_instance, &deviceCount, physicalDevices.data());
	for (VkPhysicalDevice pd : physicalDevices)
	{
		if (!IsDeviceSuitable(probeSurface, pd))
			continue;
		VkPhysicalDeviceIDProperties idProps;
		const VkPhysicalDeviceProperties2 props = QueryDeviceIdProperties(pd, idProps);
		if (firstSuitable == VK_NULL_HANDLE)
		{
			firstSuitable = pd;
			firstSuitableName = props.properties.deviceName;
		}
		if (userPickedDevice && memcmp(config.vk_graphic_device_uuid.data(), idProps.deviceUUID, VK_UUID_SIZE) != 0)
			continue;
		m_physicalDevice = pd;
		m_selectedDeviceName = props.properties.deviceName;
		break;
	}
	if (m_physicalDevice == VK_NULL_HANDLE)
	{
		if (firstSuitable == VK_NULL_HANDLE)
		{
			cemuLog_log(LogType::Force, "No physical GPU could be found with the required extensions and swap chain support.");
			throw std::runtime_error("No physical GPU could be found with the required extensions and swap chain support.");
		}
		cemuLog_log(LogType::Force, "The selected GPU could not be found or is not suitable. Falling back to first available device instead");
		m_physicalDevice = firstSuitable;
		m_selectedDeviceName = firstSuitableName;
		config.vk_graphic_device_uuid = {}; // forget the stale selection
	}

	CheckDeviceExtensionSupport(m_physicalDevice, m_featureControl);
	DetermineVendor();
	GetDeviceFeatures();

	memoryManager.reset(new VKRMemoryManager(this));

	try
	{
		VkPhysicalDeviceIDProperties idProps;
		QueryDeviceIdProperties(m_physicalDevice, idProps);
#if BOOST_OS_WINDOWS
		m_dxgi_wrapper = std::make_unique<DXGIWrapper>(idProps.deviceLUID);
#endif
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't create dxgi wrapper: {}", ex.what());
	}

	// ---- logical device ----
	m_indices = FindQueueFamilies(probeSurface, m_physicalDevice);
	const std::vector<VkDeviceQueueCreateInfo> queueInfos = CreateQueueCreateInfos({ m_indices.graphicsFamily, m_indices.presentFamily });

	VkPhysicalDeviceFeatures2 available{};
	available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	vkGetPhysicalDeviceFeatures2(m_physicalDevice, &available);

	VkPhysicalDeviceFeatures enabled{};
	enabled.independentBlend = VK_TRUE;
	enabled.samplerAnisotropy = VK_TRUE;
	enabled.imageCubeArray = VK_TRUE;
	enabled.occlusionQueryPrecise = VK_TRUE;
	enabled.depthClamp = VK_TRUE;
	enabled.depthBiasClamp = VK_TRUE;
#if !BOOST_OS_MACOS
	enabled.geometryShader = VK_TRUE;
#endif
	enabled.logicOp = available.features.logicOp; // MoltenVK exposes it only in its private-API build
	if (!enabled.logicOp)
	{
		cemuLog_log(LogType::Force, "LogicOp not supported by the driver, some rendering issues might occur");
#if BOOST_OS_MACOS
		cemuLog_log(LogType::Force, "Install the privateapi variant of MoltenVK to get logicOp support on macOS");
#endif
	}
	if (m_featureControl.deviceExtensions.pipeline_robustness)
		enabled.robustBufferAccess = VK_FALSE; // robustness is requested per pipeline instead
	else
	{
		cemuLog_log(LogType::Force, "VK_EXT_pipeline_robustness not supported. Falling back to robustBufferAccess");
		enabled.robustBufferAccess = VK_TRUE;
	}
	enabled.vertexPipelineStoresAndAtomics = available.features.vertexPipelineStoresAndAtomics;
	if (!enabled.vertexPipelineStoresAndAtomics)
		cemuLog_log(LogType::Force, "vertexPipelineStoresAndAtomics not supported by the driver. Games which use the streamout feature will not render correctly");

	// extension features switched on at device creation
	const auto& ext = m_featureControl.deviceExtensions;
	StructChain enableChain;
	VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT enCacheControl{};
	if (ext.pipeline_creation_cache_control)
		enableChain.Link(enCacheControl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT).pipelineCreationCacheControl = VK_TRUE;
	VkPhysicalDeviceCustomBorderColorFeaturesEXT enBorderColor{};
	if (ext.custom_border_color_without_format)
	{
		enableChain.Link(enBorderColor, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
		enBorderColor.customBorderColors = VK_TRUE;
		enBorderColor.customBorderColorWithoutFormat = VK_TRUE;
	}
	VkPhysicalDevicePresentIdFeaturesKHR enPresentId{};
	VkPhysicalDevicePresentWaitFeaturesKHR enPresentWait{};
	if (ext.present_wait)
	{
		enableChain.Link(enPresentId, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR).presentId = VK_TRUE;
		enableChain.Link(enPresentWait, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR).presentWait = VK_TRUE;
	}
	VkPhysicalDevicePipelineRobustnessFeaturesEXT enRobustness{};
	if (ext.pipeline_robustness)
		enableChain.Link(enRobustness, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT).pipelineRobustness = VK_TRUE;
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT enFeedbackLayout{};
	VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT enFeedbackDynState{};
	if (UseAttachmentFeedbackLoop())
	{
		enableChain.Link(enFeedbackLayout, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT).attachmentFeedbackLoopLayout = VK_TRUE;
		enableChain.Link(enFeedbackDynState, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT).attachmentFeedbackLoopDynamicState = VK_TRUE;
	}

	std::vector<const char*> deviceExtensions;
	const VkDeviceCreateInfo deviceInfo = CreateDeviceCreateInfo(queueInfos, enabled, enableChain.Head(), deviceExtensions);
	r = vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_logicalDevice);
	if (r != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan: Unable to create a logical device. Error {}", (sint32)r);
		throw std::runtime_error(fmt::format("Unable to create a logical device: {}", r));
	}
	InitializeDeviceVulkan(m_logicalDevice);

	// presentation goes through the graphics queue as well
	vkGetDeviceQueue(m_logicalDevice, m_indices.graphicsFamily, 0, &m_graphicsQueue);
	vkGetDeviceQueue(m_logicalDevice, m_indices.graphicsFamily, 0, &m_presentQueue);
	vkDestroySurfaceKHR(m_instance, probeSurface, nullptr);

	if (validation && m_featureControl.instanceExtensions.debug_utils)
	{
		auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
		VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
		messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messengerInfo.pfnUserCallback = &DebugUtilsCallback;
		createMessenger(m_instance, &messengerInfo, nullptr, &m_debugCallback);
		cemuLog_log(LogType::Force, "Debug: Vulkan validation layer enabled, vkCreateDebugUtilsMessengerEXT will be used to log validation errors");
	}
	if (IsTracingToolEnabled())
		cemuLog_log(LogType::Force, "Debug: Tracing tool detected, will recompile all shaders with debug info enabled. This disables the SPIR-V cache.");
	if (IsDebugMarkersEnabled())
		cemuLog_log(LogType::Force, "Debug: Detected tool capable of using debug markers, will use vkDebugMarkerSetObjectNameEXT to identify Vulkan objects");

	// placeholder viewport and scissor until the first draw sets real ones
	m_state.currentViewport.width = 4;
	m_state.currentViewport.height = 4;
	m_state.currentScissorRect.extent.width = 4;
	m_state.currentScissorRect.extent.height = 4;

	QueryMemoryInfo();
	QueryAvailableFormats();
	CreateCommandPool();
	CreateCommandBuffers();
	CreateDescriptorPool();
	swapchain_createDescriptorSetLayout();

	// ---- persistent buffers ----
	constexpr VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	constexpr VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	constexpr VkMemoryPropertyFlags deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	// uniform ring buffer: prefer cached coherent memory, settle for plain host visible memory
	const VkMemoryPropertyFlags uniformCandidates[] = {
		hostVisible | coherent | cached,
		hostVisible | coherent | cached | deviceLocal, // unified memory
		hostVisible | coherent,
		hostVisible | coherent | deviceLocal,
	};
	m_uniformVarBufferMemoryIsCoherent = false;
	for (VkMemoryPropertyFlags props : uniformCandidates)
	{
		if (memoryManager->CreateBuffer(UNIFORMVAR_RINGBUFFER_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, props, m_uniformVarBuffer, m_uniformVarBufferMemory))
		{
			m_uniformVarBufferMemoryIsCoherent = true;
			break;
		}
	}
	if (!m_uniformVarBufferMemoryIsCoherent)
	{
		memoryManager->CreateBuffer(UNIFORMVAR_RINGBUFFER_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostVisible, m_uniformVarBuffer, m_uniformVarBufferMemory);
		cemuLog_log(LogType::Force, "[Vulkan-Info] Using non-coherent memory for uniform data");
	}
	m_uniformVarBufferPtr = (uint8*)MapWholeMemory(m_logicalDevice, m_uniformVarBufferMemory);

	// texture readback staging
	if (!memoryManager->CreateBuffer(TEXTURE_READBACK_SIZE, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hostVisible | coherent | cached, m_textureReadbackBuffer, m_textureReadbackBufferMemory))
		memoryManager->CreateBuffer(TEXTURE_READBACK_SIZE, VK_BUFFER_USAGE_TRANSFER_DST_BIT, deviceLocal | hostVisible | cached, m_textureReadbackBuffer, m_textureReadbackBufferMemory);
	m_textureReadbackBufferPtr = (uint8*)MapWholeMemory(m_logicalDevice, m_textureReadbackBufferMemory);

	// transform feedback ring (device memory)
	memoryManager->CreateBuffer(LatteStreamout_GetRingBufferSize(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, m_xfbRingBuffer, m_xfbRingBufferMemory);

	// occlusion query results, one uint64 per pool slot
	const VkDeviceSize queryResultSize = OCCLUSION_QUERY_POOL_SIZE * sizeof(uint64);
	if (!memoryManager->CreateBuffer(queryResultSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hostVisible | coherent | cached, m_occlusionQueries.bufferQueryResults, m_occlusionQueries.memoryQueryResults))
		memoryManager->CreateBuffer(queryResultSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, deviceLocal | hostVisible | cached, m_occlusionQueries.bufferQueryResults, m_occlusionQueries.memoryQueryResults);
	m_occlusionQueries.ptrQueryResults = (uint64*)MapWholeMemory(m_logicalDevice, m_occlusionQueries.memoryQueryResults);
	for (sint32 slot = 0; slot < OCCLUSION_QUERY_POOL_SIZE; slot++)
		m_occlusionQueries.list_availableQueryIndices.emplace_back(slot);

	// background compilers for shaders and pipelines
	RendererShaderVk::Init();
	PipelineCompiler::CompileThreadPool_Start();
}

VulkanRenderer::~VulkanRenderer()
{
	// let the GPU drain before anything is released
	SubmitCommandBuffer();
	WaitDeviceIdle();
	WaitCommandBufferFinished(GetCurrentCommandBufferId());
	recorder_releaseResources();

	// stop the driver pipeline cache writer
	m_destructionRequested = true;
	m_pipeline_cache_semaphore.notify();
	m_pipeline_cache_save_thread.join();
	vkDestroyPipelineCache(m_logicalDevice, m_pipeline_cache, nullptr);

	// backbuffer blit objects
	if (!m_backbufferBlitDescriptorSetCache.empty())
	{
		std::vector<VkDescriptorSet> sets;
		sets.reserve(m_backbufferBlitDescriptorSetCache.size());
		for (const auto& entry : m_backbufferBlitDescriptorSetCache)
			sets.push_back(entry.second);
		vkFreeDescriptorSets(m_logicalDevice, m_descriptorPool, (uint32)sets.size(), sets.data());
	}
	vkDestroyDescriptorPool(m_logicalDevice, m_descriptorPool, nullptr);
	for (const auto& entry : m_backbufferBlitPipelineCache)
		vkDestroyPipeline(m_logicalDevice, entry.second, nullptr);
	m_backbufferBlitPipelineCache = {};

	if (m_occlusionQueries.queryPool != VK_NULL_HANDLE)
		vkDestroyQueryPool(m_logicalDevice, m_occlusionQueries.queryPool, nullptr);
	vkDestroyDescriptorSetLayout(m_logicalDevice, m_swapchainDescriptorSetLayout, nullptr);

	ImGui_ImplVulkan_Shutdown();
	DeleteNullObjects();

	memoryManager->DeleteBuffer(m_uniformVarBuffer, m_uniformVarBufferMemory);
	memoryManager->DeleteBuffer(m_textureReadbackBuffer, m_textureReadbackBufferMemory);
	memoryManager->DeleteBuffer(m_xfbRingBuffer, m_xfbRingBufferMemory);
	memoryManager->DeleteBuffer(m_occlusionQueries.bufferQueryResults, m_occlusionQueries.memoryQueryResults);
	memoryManager->DeleteBuffer(m_bufferCache, m_bufferCacheMemory);

	m_padSwapchainInfo = nullptr;
	m_mainSwapchainInfo = nullptr;

	surfaceCopy_cleanup();

	for (RendererShaderVk** shader : { &defaultShaders.copySurface_vs, &defaultShaders.copySurface_psColor2Depth, &defaultShaders.copySurface_psDepth2Color })
	{
		delete *shader;
		*shader = nullptr;
	}

	for (VkFence& fence : m_cmdBufferFences)
	{
		vkDestroyFence(m_logicalDevice, fence, nullptr);
		fence = VK_NULL_HANDLE;
	}
	for (VkSemaphore& semaphore : m_commandBufferSemaphores)
	{
		vkDestroySemaphore(m_logicalDevice, semaphore, nullptr);
		semaphore = VK_NULL_HANDLE;
	}
	if (m_pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_logicalDevice, m_pipelineLayout, nullptr);
	if (m_commandPool != VK_NULL_HANDLE)
		vkDestroyCommandPool(m_logicalDevice, m_commandPool, nullptr);

	VKRObjectSampler::DestroyCache();

	if (m_debugCallback)
	{
		auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
		destroyMessenger(m_instance, m_debugCallback, nullptr);
	}

	while (!m_destructionQueue.empty())
		ProcessDestructionQueue();

	memoryManager.reset();

	if (m_instance != VK_NULL_HANDLE)
	{
		if (m_logicalDevice != VK_NULL_HANDLE)
			vkDestroyDevice(m_logicalDevice, nullptr);
		vkDestroyInstance(m_instance, nullptr);
	}
	// glslang::FinalizeProcess() is deliberately not called, it is not safe to run at this point
}

VulkanRenderer* VulkanRenderer::GetInstance()
{
	cemu_assert_debug(g_renderer->GetType() == RendererAPI::Vulkan);
	return static_cast<VulkanRenderer*>(g_renderer.get());
}

void VulkanRenderer::InitializeSurface(const Vector2i& size, bool mainWindow)
{
	std::unique_ptr<SwapchainInfoVk>& slot = mainWindow ? m_mainSwapchainInfo : m_padSwapchainInfo;
	slot = std::make_unique<SwapchainInfoVk>(mainWindow, size);
	slot->Create();
}

const std::unique_ptr<SwapchainInfoVk>& VulkanRenderer::GetChainInfoPtr(bool mainWindow) const
{
	return mainWindow ? m_mainSwapchainInfo : m_padSwapchainInfo;
}

SwapchainInfoVk& VulkanRenderer::GetChainInfo(bool mainWindow) const
{
	return *GetChainInfoPtr(mainWindow);
}

void VulkanRenderer::StopUsingPadAndWait()
{
	// the render thread tears the pad swapchain down on its next acquire and clears the flag
	m_destroyPadSwapchainNextAcquire.test_and_set();
	m_destroyPadSwapchainNextAcquire.wait(true);
}

bool VulkanRenderer::IsPadWindowActive()
{
	return IsSwapchainInfoValid(false);
}

void VulkanRenderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView)
{
	if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
		return;

	if (IsSwapchainInfoValid(false))
	{
		// with the GamePad window open a request produces one picture per window;
		// the state remembers which of the two is still outstanding
		if (m_screenshot_state == ScreenshotState::Main && padView)
			return;
		if (m_screenshot_state == ScreenshotState::Pad && !padView)
			return;
		if (m_screenshot_state == ScreenshotState::None)
			m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad;
		else
			m_screenshot_state = ScreenshotState::None;
	}
	else
		m_screenshot_state = ScreenshotState::None;

	LatteTextureViewVk* viewVk = (LatteTextureViewVk*)texView;
	LatteTextureVk* source = viewVk->GetBaseImage();
	VKRObjectTexture* sourceObj = source->GetImageObj();
	sourceObj->flagForCurrentCommandBuffer();
	const VkImage sourceImage = sourceObj->m_image;

	int width, height;
	source->GetEffectiveSize(width, height, 0);

	if (viewVk->firstMip != 0)
	{
		cemuLog_log(LogType::Force, "Failed to capture screenshot: capturing non-zero mip is not supported");
		return;
	}

	VkFormat format = source->GetFormat();
	VkImage readImage = sourceImage; // image the pixels are copied out of
	VkImage convertImage = VK_NULL_HANDLE;
	VkDeviceMemory convertMemory = VK_NULL_HANDLE;

	const bool copyAsIs = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
		format == VK_FORMAT_R8G8B8_UNORM || format == VK_FORMAT_R8G8B8_SNORM;
	if (!copyAsIs)
	{
		// other formats are converted to 8 bit RGBA with a blit into a temporary image
		const bool targetSRGB = padView ? LatteGPUState.drcBufferUsesSRGB : LatteGPUState.tvBufferUsesSRGB;
		const VkFormat convertFormat = targetSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &fp);
		bool canBlit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, convertFormat, &fp);
		canBlit = canBlit && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
		if (!canBlit)
		{
			cemuLog_log(LogType::Force, "Screenshot failed: Framebuffer is not in RGB8 format and blitting is unsupported");
			return;
		}

		VkImageCreateInfo ici{};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = convertFormat;
		ici.extent = { (uint32)width, (uint32)height, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(m_logicalDevice, &ici, nullptr, &convertImage) != VK_SUCCESS)
			return;

		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(m_logicalDevice, convertImage, &req);
		uint32 typeIndex;
		if (!memoryManager->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
		{
			vkDestroyImage(m_logicalDevice, convertImage, nullptr);
			cemuLog_log(LogType::Force, "Screenshot request failed due to incompatible vulkan memory types.");
			return;
		}
		VkMemoryAllocateInfo mai{};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = typeIndex;
		if (vkAllocateMemory(m_logicalDevice, &mai, nullptr, &convertMemory) != VK_SUCCESS)
		{
			vkDestroyImage(m_logicalDevice, convertImage, nullptr);
			cemuLog_log(LogType::Force, "Screenshot request failed due to failed memory allocation.");
			return;
		}
		vkBindImageMemory(m_logicalDevice, convertImage, convertMemory, 0);

		const VkImageSubresourceRange convertRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		const VkImageSubresourceLayers sourceLayer{ VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32)viewVk->firstSlice, 1 };
		// a source stage is needed even though nothing touched the new image (no synchronization2 here)
		barrier_image<TRANSFER_READ, TRANSFER_WRITE>(convertImage, convertRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		barrier_image<IMAGE_WRITE | TRANSFER_WRITE, TRANSFER_READ>(source, sourceLayer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		VkImageBlit blit{};
		blit.srcSubresource = sourceLayer;
		blit.srcOffsets[1] = { width, height, 1 };
		blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		blit.dstOffsets[1] = { width, height, 1 };
		vkCmdBlitImage(m_state.currentCommandBuffer, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, convertImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

		barrier_image<TRANSFER_WRITE, TRANSFER_READ>(convertImage, convertRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		const VkImageSubresourceLayers restoreLayer{ source->GetImageAspect(), 0, (uint32)viewVk->firstSlice, 1 };
		barrier_image<TRANSFER_READ, TRANSFER_WRITE | IMAGE_WRITE>(source, restoreLayer, source->GetDefaultLayout());

		// the converted bytes are taken as they are, whichever of the two RGBA8 formats was used
		format = VK_FORMAT_R8G8B8A8_UNORM;
		readImage = convertImage;
	}

	uint32 bytesPerPixel = 0;
	if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB)
		bytesPerPixel = 4;
	else if (format == VK_FORMAT_R8G8B8_UNORM || format == VK_FORMAT_R8G8B8_SRGB)
		bytesPerPixel = 3;
	if (bytesPerPixel == 0)
	{
		cemu_assert_debug(false);
		return;
	}
	const uint32 byteCount = bytesPerPixel * width * height;

	VkBuffer hostBuffer = nullptr;
	VkDeviceMemory hostMemory = nullptr;
	memoryManager->CreateBuffer(byteCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, hostBuffer, hostMemory);
	const uint8* pixels = (const uint8*)MapWholeMemory(m_logicalDevice, hostMemory);

	VkBufferImageCopy copy{};
	copy.bufferRowLength = width;
	copy.bufferImageHeight = height;
	copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copy.imageExtent = { (uint32)width, (uint32)height, 1 };
	const bool readsSource = readImage == sourceImage;
	if (readsSource)
	{
		// reading the guest surface directly: pick its slice and order the copy after its writers
		copy.imageSubresource.baseArrayLayer = viewVk->firstSlice;
		barrier_image<IMAGE_WRITE | TRANSFER_WRITE, TRANSFER_READ>(source, copy.imageSubresource, VK_IMAGE_LAYOUT_GENERAL);
	}
	vkCmdCopyImageToBuffer(m_state.currentCommandBuffer, readImage, VK_IMAGE_LAYOUT_GENERAL, hostBuffer, 1, &copy);
	if (readsSource)
		barrier_image<TRANSFER_READ, TRANSFER_WRITE | IMAGE_WRITE>(source, copy.imageSubresource, source->GetDefaultLayout());

	SubmitCommandBuffer();
	WaitCommandBufferFinished(GetCurrentCommandBufferId());

	// repack as tightly packed 8 bit RGB
	std::vector<uint8> rgb;
	bool haveImage = true;
	const size_t pixelCount = (size_t)width * height;
	switch (format)
	{
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	{
		const bool decodeSRGB = format == VK_FORMAT_R8G8B8A8_SRGB;
		rgb.resize(pixelCount * 3);
		for (size_t i = 0; i < pixelCount; i++)
		{
			for (size_t c = 0; c < 3; c++)
			{
				const uint8 v = pixels[i * 4 + c];
				rgb[i * 3 + c] = decodeSRGB ? SRGBComponentToRGB(v) : v;
			}
		}
		break;
	}
	case VK_FORMAT_R8G8B8_UNORM:
		rgb.assign(pixels, pixels + byteCount);
		break;
	case VK_FORMAT_R8G8B8_SRGB:
		rgb.resize(byteCount);
		for (uint32 i = 0; i < byteCount; i++)
			rgb[i] = SRGBComponentToRGB(pixels[i]);
		break;
	default:
		haveImage = false;
		cemu_assert_debug(false);
		break;
	}

	vkUnmapMemory(m_logicalDevice, hostMemory);
	vkFreeMemory(m_logicalDevice, hostMemory, nullptr);
	vkDestroyBuffer(m_logicalDevice, hostBuffer, nullptr);
	if (convertImage)
		vkDestroyImage(m_logicalDevice, convertImage, nullptr);
	if (convertMemory)
		vkFreeMemory(m_logicalDevice, convertMemory, nullptr);

	if (haveImage)
		SaveScreenshot(rgb, width, height, !padView);
}

namespace
{
	const float s_queuePriorityOne = 1.0f;
}

std::vector<VkDeviceQueueCreateInfo> VulkanRenderer::CreateQueueCreateInfos(const std::set<sint32>& uniqueQueueFamilies) const
{
	std::vector<VkDeviceQueueCreateInfo> infos;
	infos.reserve(uniqueQueueFamilies.size());
	for (sint32 family : uniqueQueueFamilies)
	{
		VkDeviceQueueCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		info.queueFamilyIndex = family;
		info.queueCount = 1;
		info.pQueuePriorities = &s_queuePriorityOne;
		infos.push_back(info);
	}
	return infos;
}

VkDeviceCreateInfo VulkanRenderer::CreateDeviceCreateInfo(const std::vector<VkDeviceQueueCreateInfo>& queueCreateInfos, const VkPhysicalDeviceFeatures& deviceFeatures, const void* deviceExtensionStructs, std::vector<const char*>& used_extensions) const
{
	const auto& ext = m_featureControl.deviceExtensions;
	used_extensions.assign(std::begin(s_mandatoryDeviceExtensions), std::end(s_mandatoryDeviceExtensions));
	const std::pair<bool, const char*> optional[] = {
		{ ext.tooling_info, VK_EXT_TOOLING_INFO_EXTENSION_NAME },
		{ ext.depth_range_unrestricted, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME },
		{ ext.nv_fill_rectangle, VK_NV_FILL_RECTANGLE_EXTENSION_NAME },
		{ ext.pipeline_feedback, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME },
		{ ext.cubic_filter, VK_EXT_FILTER_CUBIC_EXTENSION_NAME },
		{ ext.custom_border_color, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME },
		{ ext.driver_properties, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME },
		{ ext.external_memory_host, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME },
		{ ext.synchronization2, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME },
		{ ext.dynamic_rendering, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME },
		{ ext.shader_float_controls, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME },
		{ ext.depth_clip_enable, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME },
		{ ext.present_wait, VK_KHR_PRESENT_ID_EXTENSION_NAME },
		{ ext.present_wait, VK_KHR_PRESENT_WAIT_EXTENSION_NAME },
		{ ext.pipeline_robustness, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME },
		{ UseAttachmentFeedbackLoop(), VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME },
		{ UseAttachmentFeedbackLoop(), VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME },
	};
	for (const auto& [wanted, name] : optional)
	{
		if (wanted)
			used_extensions.emplace_back(name);
	}

	VkDeviceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.pNext = deviceExtensionStructs;
	info.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();
	info.pQueueCreateInfos = queueCreateInfos.data();
	info.enabledExtensionCount = (uint32_t)used_extensions.size();
	info.ppEnabledExtensionNames = used_extensions.data();
	info.pEnabledFeatures = &deviceFeatures;
	if (!m_layerNames.empty())
	{
		info.enabledLayerCount = (uint32_t)m_layerNames.size();
		info.ppEnabledLayerNames = m_layerNames.data();
	}
	return info;
}

RendererShader* VulkanRenderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackShader)
{
	return new RendererShaderVk(type, baseHash, auxHash, isGameShader, isGfxPackShader, source);
}

VulkanRenderer::QueueFamilyIndices VulkanRenderer::FindQueueFamilies(VkSurfaceKHR surface, VkPhysicalDevice device)
{
	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

	// the last family seen with each capability wins, the scan stops once both are known
	QueueFamilyIndices found;
	for (uint32_t index = 0; index < familyCount && !found.IsComplete(); index++)
	{
		const VkQueueFamilyProperties& family = families[index];
		if (family.queueCount > 0 && (family.queueFlags & VK_QUEUE_GRAPHICS_BIT))
			found.graphicsFamily = (int32_t)index;

		VkBool32 canPresent = VK_FALSE;
		const VkResult r = vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &canPresent);
		if (r != VK_SUCCESS)
			throw std::runtime_error(fmt::format("Error while attempting to check if a surface supports presentation: {}", r));
		if (family.queueCount > 0 && canPresent)
			found.presentFamily = (int32_t)index;
	}
	return found;
}

bool VulkanRenderer::CheckDeviceExtensionSupport(const VkPhysicalDevice device, FeatureControl& info)
{
	uint32_t count;
	VkResult r = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
	if (r != VK_SUCCESS)
		throw std::runtime_error(fmt::format("Cannot retrieve count of properties for a physical device: {}", r));
	std::vector<VkExtensionProperties> available(count);
	r = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
	if (r != VK_SUCCESS)
		throw std::runtime_error(fmt::format("Cannot retrieve properties for a physical device: {}", r));

	bool allMandatory = true;
	for (const char* name : s_mandatoryDeviceExtensions)
		allMandatory = allMandatory && HasExtension(available, name);

	auto& ext = info.deviceExtensions;
	auto has = [&available](const char* name) { return HasExtension(available, name); };
	ext.tooling_info = has(VK_EXT_TOOLING_INFO_EXTENSION_NAME);
	ext.depth_range_unrestricted = has(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
	ext.nv_fill_rectangle = has(VK_NV_FILL_RECTANGLE_EXTENSION_NAME);
	ext.pipeline_feedback = has(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
	ext.cubic_filter = has(VK_EXT_FILTER_CUBIC_EXTENSION_NAME);
	ext.custom_border_color = has(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
	ext.driver_properties = has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
	ext.external_memory_host = has(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
	ext.synchronization2 = has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
	ext.shader_float_controls = has(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
	ext.dynamic_rendering = false; // not used: current drivers gain nothing from it
	ext.depth_clip_enable = has(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
	ext.pipeline_robustness = has(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
	ext.attachment_feedback_loop_layout = has(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
	ext.attachment_feedback_loop_dynamic_state = has(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
	ext.present_wait = has(VK_KHR_PRESENT_WAIT_EXTENSION_NAME) && has(VK_KHR_PRESENT_ID_EXTENSION_NAME);

	// frame debuggers and tracing tools announce themselves through VK_EXT_tooling_info
	info.usingDebugMarkerTool = false;
	info.usingTracingTool = false;
	if (ext.tooling_info && vkGetPhysicalDeviceToolPropertiesEXT)
	{
		uint32_t toolCount = 0;
		if (vkGetPhysicalDeviceToolPropertiesEXT(device, &toolCount, nullptr) == VK_SUCCESS)
		{
			std::vector<VkPhysicalDeviceToolPropertiesEXT> tools(toolCount);
			if (toolCount > 0 && vkGetPhysicalDeviceToolPropertiesEXT(device, &toolCount, tools.data()) == VK_SUCCESS)
			{
				for (const auto& tool : tools)
				{
					if ((tool.purposes & VK_TOOL_PURPOSE_DEBUG_MARKERS_BIT_EXT) && info.instanceExtensions.debug_utils && vkSetDebugUtilsObjectNameEXT)
						info.usingDebugMarkerTool = true;
					if (tool.purposes & VK_TOOL_PURPOSE_TRACING_BIT)
						info.usingTracingTool = true;
				}
			}
		}
	}
	return allMandatory;
}

std::vector<const char*> VulkanRenderer::CheckInstanceExtensionSupport(FeatureControl& info)
{
	uint32_t count;
	VkResult r = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	if (r != VK_SUCCESS)
		throw std::runtime_error(fmt::format("Failed to retrieve the instance extension properties : {}", r));
	std::vector<VkExtensionProperties> available(count);
	r = vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
	if (r != VK_SUCCESS)
		throw std::runtime_error(fmt::format("Failed to retrieve the instance extension properties: {}", r));

	std::vector<const char*> missing;
	AddSurfaceExtensions(missing);
	if (cemuLog_isLoggingEnabled(LogType::VulkanValidation))
		missing.emplace_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

	// enable the required extensions in the order the loader lists them
	std::vector<const char*> enabled;
	for (const VkExtensionProperties& prop : available)
	{
		auto it = std::find_if(missing.begin(), missing.end(), [&prop](const char* name) { return strcmp(name, prop.extensionName) == 0; });
		if (it == missing.end())
			continue;
		enabled.emplace_back(*it);
		missing.erase(it);
	}
	if (!missing.empty())
	{
		cemuLog_log(LogType::Force, "The following required Vulkan instance extensions are not supported:");
		for (const char* name : missing)
			cemuLog_log(LogType::Force, "{}", name);
		cemuLog_waitForFlush();
		throw std::runtime_error(std::string()); // the details went to the log
	}

	info.instanceExtensions.debug_utils = HasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (info.instanceExtensions.debug_utils)
		enabled.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	return enabled;
}

bool VulkanRenderer::IsDeviceSuitable(VkSurfaceKHR surface, const VkPhysicalDevice& device)
{
	if (!FindQueueFamilies(surface, device).IsComplete())
		return false;

	// Vulkan 1.1 is the minimum; read the version the 1.0 way
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(device, &props);
	const uint32 major = VK_API_VERSION_MAJOR(props.apiVersion);
	const uint32 minor = VK_API_VERSION_MINOR(props.apiVersion);
	if (major < 1 || (major == 1 && minor < 1))
		return false;

	FeatureControl scratch;
	if (!CheckDeviceExtensionSupport(device, scratch))
		return false;

	const auto support = SwapchainInfoVk::QuerySwapchainSupport(surface, device);
	return !support.formats.empty() && !support.presentModes.empty();
}

namespace
{
	template<typename TInfo, typename TCreate>
	VkSurfaceKHR CreateSurfaceOrThrow(VkInstance instance, const TInfo& info, TCreate create, const char* what)
	{
		VkSurfaceKHR surface;
		const VkResult r = create(instance, &info, nullptr, &surface);
		if (r != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "Cannot create a {} Vulkan surface: {}", what, (sint32)r);
			throw std::runtime_error(fmt::format("Cannot create a {} Vulkan surface: {}", what, r));
		}
		return surface;
	}
}

#if BOOST_OS_WINDOWS
VkSurfaceKHR VulkanRenderer::CreateWinSurface(VkInstance instance, HWND hwindow)
{
	VkWin32SurfaceCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	info.hinstance = GetModuleHandle(nullptr);
	info.hwnd = hwindow;
	return CreateSurfaceOrThrow(instance, info, vkCreateWin32SurfaceKHR, "Win32");
}
#endif

#if BOOST_OS_LINUX || BOOST_OS_BSD
VkSurfaceKHR VulkanRenderer::CreateXlibSurface(VkInstance instance, Display* dpy, Window window)
{
	VkXlibSurfaceCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
	info.dpy = dpy;
	info.window = window;
	return CreateSurfaceOrThrow(instance, info, vkCreateXlibSurfaceKHR, "X11");
}

VkSurfaceKHR VulkanRenderer::CreateXcbSurface(VkInstance instance, xcb_connection_t* connection, xcb_window_t window)
{
	VkXcbSurfaceCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
	info.connection = connection;
	info.window = window;
	return CreateSurfaceOrThrow(instance, info, vkCreateXcbSurfaceKHR, "XCB");
}

#ifdef HAS_WAYLAND
VkSurfaceKHR VulkanRenderer::CreateWaylandSurface(VkInstance instance, wl_display* display, wl_surface* surface)
{
	VkWaylandSurfaceCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	info.display = display;
	info.surface = surface;
	return CreateSurfaceOrThrow(instance, info, vkCreateWaylandSurfaceKHR, "Wayland");
}
#endif
#endif

VkSurfaceKHR VulkanRenderer::CreateFramebufferSurface(VkInstance instance, WindowSystem::WindowHandleInfo& windowInfo)
{
#if BOOST_OS_WINDOWS
	return CreateWinSurface(instance, static_cast<HWND>(windowInfo.surface));
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	switch (windowInfo.backend)
	{
	case WindowSystem::WindowHandleInfo::Backend::X11:
		return CreateXlibSurface(instance, static_cast<Display*>(windowInfo.display), reinterpret_cast<Window>(windowInfo.surface));
#ifdef HAS_WAYLAND
	case WindowSystem::WindowHandleInfo::Backend::Wayland:
		return CreateWaylandSurface(instance, static_cast<wl_display*>(windowInfo.display), static_cast<wl_surface*>(windowInfo.surface));
#endif
	default:
		return {};
	}
#elif BOOST_OS_MACOS
	return CreateCocoaSurface(instance, windowInfo.surface);
#endif
}

void VulkanRenderer::CreateCommandPool()
{
	VkCommandPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	info.queueFamilyIndex = m_indices.graphicsFamily;
	const VkResult r = vkCreateCommandPool(m_logicalDevice, &info, nullptr, &m_commandPool);
	if (r != VK_SUCCESS)
		throw std::runtime_error(fmt::format("Failed to create command pool: {}", r));
}

void VulkanRenderer::CreateCommandBuffers()
{
	// one fence per ring slot, created unsignaled
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	for (VkFence& fence : m_cmdBufferFences)
		vkCreateFence(m_logicalDevice, &fenceInfo, nullptr, &fence);

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = (uint32_t)m_commandBuffers.size();
	const VkResult r = vkAllocateCommandBuffers(m_logicalDevice, &allocInfo, m_commandBuffers.data());
	if (r != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to allocate command buffers: {}", r);
		throw std::runtime_error(fmt::format("Failed to allocate command buffers: {}", r));
	}

	// each submission signals its slot's semaphore, the next submission waits on it
	VkSemaphoreCreateInfo semInfo{};
	semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (VkSemaphore& semaphore : m_commandBufferSemaphores)
	{
		if (vkCreateSemaphore(m_logicalDevice, &semInfo, nullptr, &semaphore) != VK_SUCCESS)
			UnrecoverableError("Failed to create semaphore for command buffer");
	}
}

bool VulkanRenderer::IsSwapchainInfoValid(bool mainWindow) const
{
	const auto& chain = GetChainInfoPtr(mainWindow);
	return chain && chain->IsValid();
}

void VulkanRenderer::CreateNullTexture(NullTexture& nullTex, VkImageType imageType)
{
	// tiny placeholder bound when a shader samples a texture unit with no texture
	cemu_assert(imageType == VK_IMAGE_TYPE_1D || imageType == VK_IMAGE_TYPE_2D);
	VkImageCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = imageType;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = { 4, 1, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateImage(m_logicalDevice, &ici, nullptr, &nullTex.image) != VK_SUCCESS)
		UnrecoverableError("Failed to create nullTex image");
	nullTex.allocation = memoryManager->imageMemoryAllocate(nullTex.image);

	const VkClearColorValue black{};
	ClearColorImageRaw(nullTex.image, 0, 0, black, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

	VkImageViewCreateInfo vci{};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = nullTex.image;
	vci.viewType = imageType == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if (vkCreateImageView(m_logicalDevice, &vci, nullptr, &nullTex.view) != VK_SUCCESS)
		UnrecoverableError("Failed to create nullTex image view");

	VkSamplerCreateInfo sci{};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.anisotropyEnable = VK_FALSE;
	sci.maxAnisotropy = 1.0f;
	sci.compareOp = VK_COMPARE_OP_NEVER;
	sci.minLod = 0.0f;
	sci.maxLod = 0.0f;
	sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	vkCreateSampler(m_logicalDevice, &sci, nullptr, &nullTex.sampler);
}

void VulkanRenderer::CreateNullObjects()
{
	CreateNullTexture(nullTexture1D, VK_IMAGE_TYPE_1D);
	CreateNullTexture(nullTexture2D, VK_IMAGE_TYPE_2D);
}

void VulkanRenderer::DeleteNullTexture(NullTexture& nullTex)
{
	vkDestroySampler(m_logicalDevice, nullTex.sampler, nullptr);
	vkDestroyImageView(m_logicalDevice, nullTex.view, nullptr);
	vkDestroyImage(m_logicalDevice, nullTex.image, nullptr);
	memoryManager->imageMemoryFree(nullTex.allocation);
	nullTex = {};
}

void VulkanRenderer::DeleteNullObjects()
{
	DeleteNullTexture(nullTexture1D);
	DeleteNullTexture(nullTexture2D);
}

void VulkanRenderer::ImguiInit()
{
	const VkRenderPass oldPass = m_imguiRenderPass;

	// imgui draws on top of the presented image, so the pass loads and keeps it in present layout
	VkAttachmentDescription target{};
	target.format = m_mainSwapchainInfo->m_surfaceFormat.format;
	target.samples = VK_SAMPLE_COUNT_1_BIT;
	target.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	target.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	target.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	target.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	target.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	const VkAttachmentReference targetRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &targetRef;
	VkRenderPassCreateInfo rpi{};
	rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpi.attachmentCount = 1;
	rpi.pAttachments = &target;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &subpass;
	const VkResult r = vkCreateRenderPass(m_logicalDevice, &rpi, nullptr, &m_imguiRenderPass);
	if (r != VK_SUCCESS)
		throw VkException(r, "can't create imgui renderpass");

	ImGui_ImplVulkan_InitInfo init{};
	init.Instance = m_instance;
	init.PhysicalDevice = m_physicalDevice;
	init.Device = m_logicalDevice;
	init.QueueFamily = m_indices.presentFamily;
	init.Queue = m_presentQueue;
	init.PipelineCache = m_pipeline_cache;
	init.DescriptorPool = m_descriptorPool;
	init.MinImageCount = m_mainSwapchainInfo->m_swapchainImages.size();
	init.ImageCount = init.MinImageCount;
	ImGui_ImplVulkan_Init(&init, m_imguiRenderPass);

	if (oldPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(GetLogicalDevice(), oldPass, nullptr);
}

void VulkanRenderer::Initialize()
{
	Renderer::Initialize();
	InitFirstCommandBuffer();
	CreatePipelineCache();
	ImguiInit();
	CreateNullObjects();
}

void VulkanRenderer::Shutdown()
{
	SubmitCommandBuffer();
	WaitDeviceIdle();
	RendererShaderVk::Shutdown();
	PipelineCompiler::CompileThreadPool_Stop();
	DeleteFontTextures();
	Renderer::Shutdown();
	if (m_imguiRenderPass != VK_NULL_HANDLE)
	{
		vkDestroyRenderPass(m_logicalDevice, m_imguiRenderPass, nullptr);
		m_imguiRenderPass = VK_NULL_HANDLE;
	}
	RendererShaderVk::Shutdown();
}

void VulkanRenderer::UnrecoverableError(const char* errMsg) const
{
	cemuLog_log(LogType::Force, "Unrecoverable error in Vulkan renderer");
	cemuLog_log(LogType::Force, "Msg: {}", errMsg);
	throw std::runtime_error(errMsg);
}

namespace
{
	// formats checked at startup; the log lists any the device lacks
	struct FormatRequirement
	{
		VkFormat format;
		const char* name;
		bool depth;
		bool renderTarget;
		bool blending;
	};

#define PIXELU_DEPTH_FMT(f) FormatRequirement{ f, #f, true, true, false }
#define PIXELU_COLOR_FMT(f, rt, blend) FormatRequirement{ f, #f, false, rt, blend }
	const FormatRequirement s_checkedFormats[] = {
		PIXELU_DEPTH_FMT(VK_FORMAT_D32_SFLOAT_S8_UINT),
		PIXELU_DEPTH_FMT(VK_FORMAT_D24_UNORM_S8_UINT),
		PIXELU_DEPTH_FMT(VK_FORMAT_D32_SFLOAT),
		PIXELU_DEPTH_FMT(VK_FORMAT_D16_UNORM),
		PIXELU_COLOR_FMT(VK_FORMAT_R32G32B32A32_SFLOAT, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R32G32B32A32_UINT, true, false),
		PIXELU_COLOR_FMT(VK_FORMAT_R16G16B16A16_SFLOAT, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R16G16B16A16_UINT, true, false),
		PIXELU_COLOR_FMT(VK_FORMAT_R16G16B16A16_UNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R16G16B16A16_SNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R8G8B8A8_UNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R8G8B8A8_SNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R8G8B8A8_SRGB, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R8G8B8A8_UINT, true, false),
		PIXELU_COLOR_FMT(VK_FORMAT_R8G8B8A8_SINT, true, false),
		PIXELU_COLOR_FMT(VK_FORMAT_R4G4B4A4_UNORM_PACK16, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R32G32_SFLOAT, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R32G32_UINT, true, false),
		PIXELU_COLOR_FMT(VK_FORMAT_R16G16_UNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R16G16_SFLOAT, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R8G8_UNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R8G8_SNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R4G4_UNORM_PACK8, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R32_SFLOAT, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R32_UINT, true, false),
		PIXELU_COLOR_FMT(VK_FORMAT_R16_SFLOAT, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R16_UNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R16_SNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R8_UNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R8_SNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R5G6B5_UNORM_PACK16, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R5G5B5A1_UNORM_PACK16, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_B10G11R11_UFLOAT_PACK32, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R16G16B16A16_SNORM, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC2_UNORM_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC2_SRGB_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC3_UNORM_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC3_SRGB_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC4_UNORM_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC4_SNORM_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC5_UNORM_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_BC5_SNORM_BLOCK, false, false),
		PIXELU_COLOR_FMT(VK_FORMAT_A2B10G10R10_UNORM_PACK32, true, true),
		PIXELU_COLOR_FMT(VK_FORMAT_R32_SFLOAT, true, true),
	};
#undef PIXELU_DEPTH_FMT
#undef PIXELU_COLOR_FMT
}

void VulkanRenderer::QueryMemoryInfo()
{
	VkPhysicalDeviceMemoryProperties mem;
	vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &mem);
	cemuLog_log(LogType::Force, "Vulkan device memory info:");
	for (uint32 h = 0; h < mem.memoryHeapCount; h++)
		cemuLog_log(LogType::Force, "Heap {} - Size {}MB Flags 0x{:08x}", h, (sint32)(mem.memoryHeaps[h].size / (1024ll * 1024ll)), (uint32)mem.memoryHeaps[h].flags);
	for (uint32 t = 0; t < mem.memoryTypeCount; t++)
		cemuLog_log(LogType::Force, "Memory {} - HeapIndex {} Flags 0x{:08x}", t, (sint32)mem.memoryTypes[t].heapIndex, (uint32)mem.memoryTypes[t].propertyFlags);
}

void VulkanRenderer::QueryAvailableFormats()
{
	auto hasOptimalTiling = [this](VkFormat format) {
		VkFormatProperties fp{};
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &fp);
		return fp.optimalTilingFeatures != 0;
	};
	// formats with fallbacks in GetTextureFormatInfoVK
	if (hasOptimalTiling(VK_FORMAT_D24_UNORM_S8_UINT))
		m_supportedFormatInfo.fmt_d24_unorm_s8_uint = true;
	if (hasOptimalTiling(VK_FORMAT_R4G4_UNORM_PACK8))
		m_supportedFormatInfo.fmt_r4g4_unorm_pack = true;
	if (hasOptimalTiling(VK_FORMAT_R5G6B5_UNORM_PACK16))
		m_supportedFormatInfo.fmt_r5g6b5_unorm_pack = true;
	if (hasOptimalTiling(VK_FORMAT_R4G4B4A4_UNORM_PACK16))
		m_supportedFormatInfo.fmt_r4g4b4a4_unorm_pack = true;
	if (hasOptimalTiling(VK_FORMAT_A1R5G5B5_UNORM_PACK16))
		m_supportedFormatInfo.fmt_a1r5g5b5_unorm_pack = true;

	// only a format with no optimal tiling support at all is reported
	for (const FormatRequirement& req : s_checkedFormats)
	{
		if (!hasOptimalTiling(req.format))
			cemuLog_log(LogType::Force, "{} not supported", req.name);
	}
}

bool VulkanRenderer::ImguiBegin(bool mainWindow)
{
	if (!Renderer::ImguiBegin(mainWindow))
		return false;
	auto& chain = GetChainInfo(mainWindow);
	if (!AcquireNextSwapchainImage(mainWindow))
		return false;

	draw_endRenderPass();
	m_state.currentPipeline = VK_NULL_HANDLE; // imgui binds its own pipeline

	ImGui_ImplVulkan_CreateFontsTexture(m_state.currentCommandBuffer);
	ImGui_ImplVulkan_NewFrame(m_state.currentCommandBuffer, chain.m_swapchainFramebuffers[chain.swapchainImageIndex], chain.getExtent());
	ImGui_UpdateWindowInformation(mainWindow);
	ImGui::NewFrame();
	return true;
}

void VulkanRenderer::ImguiEnd()
{
	ImGui::Render();
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), m_state.currentCommandBuffer);
	vkCmdEndRenderPass(m_state.currentCommandBuffer);
	// imgui changed the dynamic viewport and scissor; put back the emulator's
	vkCmdSetViewport(m_state.currentCommandBuffer, 0, 1, &m_state.currentViewport);
	vkCmdSetScissor(m_state.currentCommandBuffer, 0, 1, &m_state.currentScissorRect);
}

ImTextureID VulkanRenderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
	try
	{
		// expand RGB to RGBA with opaque alpha
		std::vector<uint8> rgba((size_t)size.x * size.y * 4);
		const size_t pixels = data.size() / 3;
		for (size_t p = 0; p < pixels; p++)
		{
			rgba[p * 4 + 0] = data[p * 3 + 0];
			rgba[p * 4 + 1] = data[p * 3 + 1];
			rgba[p * 4 + 2] = data[p * 3 + 2];
			rgba[p * 4 + 3] = 0xFF;
		}
		return (ImTextureID)ImGui_ImplVulkan_GenerateTexture(m_state.currentCommandBuffer, rgba, size);
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't generate imgui texture: {}", ex.what());
		return nullptr;
	}
}

void VulkanRenderer::DeleteTexture(ImTextureID id)
{
	WaitDeviceIdle();
	ImGui_ImplVulkan_DeleteTexture(id);
}

void VulkanRenderer::DeleteFontTextures()
{
	WaitDeviceIdle();
	ImGui_ImplVulkan_DestroyFontsTexture();
}

bool VulkanRenderer::BeginFrame(bool mainWindow)
{
	if (!AcquireNextSwapchainImage(mainWindow))
		return false;
	auto& chain = GetChainInfo(mainWindow);
	const VkClearColorValue black{};
	ClearColorImageRaw(chain.m_swapchainImages[chain.swapchainImageIndex], 0, 0, black, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	chain.hasDefinedSwapchainImage = true;
	return true;
}

void VulkanRenderer::DrawEmptyFrame(bool mainWindow)
{
	if (BeginFrame(mainWindow))
		SwapBuffers(mainWindow, !mainWindow);
}

namespace
{
	void BeginOneTimeCommandBuffer(VkCommandBuffer cmd)
	{
		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &begin);
	}
}

void VulkanRenderer::InitFirstCommandBuffer()
{
	cemu_assert_debug(m_state.currentCommandBuffer == nullptr);
	m_commandBufferIndex = 0;
	m_commandBufferSyncIndex = 0;
	m_state.currentCommandBuffer = m_commandBuffers[0];
	vkResetFences(m_logicalDevice, 1, &m_cmdBufferFences[0]);
	BeginOneTimeCommandBuffer(m_state.currentCommandBuffer);
	vkCmdSetViewport(m_state.currentCommandBuffer, 0, 1, &m_state.currentViewport);
	vkCmdSetScissor(m_state.currentCommandBuffer, 0, 1, &m_state.currentScissorRect);
	m_state.resetCommandBufferState();
}

void VulkanRenderer::ProcessFinishedCommandBuffers()
{
	bool anyRetired = false;
	// retire submitted buffers oldest first until one is still running
	while (m_commandBufferSyncIndex != m_commandBufferIndex)
	{
		const VkResult status = vkGetFenceStatus(m_logicalDevice, m_cmdBufferFences[m_commandBufferSyncIndex]);
		if (status == VK_NOT_READY)
			break;
		if (status != VK_SUCCESS)
			UnrecoverableError(fmt::format("vkGetFenceStatus returned unexpected error {}", (sint32)status).c_str());

		ProcessDestructionQueue();
		m_uniformVarBufferReadIndex = m_cmdBufferUniformRingbufIndices[m_commandBufferSyncIndex];
		m_commandBufferSyncIndex = (m_commandBufferSyncIndex + 1) % m_commandBuffers.size();
		memoryManager->cleanupBuffers(m_countCommandBufferFinished);
		m_countCommandBufferFinished++;
		anyRetired = true;
	}
	if (anyRetired)
		LatteTextureReadback_UpdateFinishedTransfers(false);
}

void VulkanRenderer::WaitForNextFinishedCommandBuffer()
{
	cemu_assert_debug(m_commandBufferSyncIndex != m_commandBufferIndex);
	const VkResult r = vkWaitForFences(m_logicalDevice, 1, &m_cmdBufferFences[m_commandBufferSyncIndex], true, UINT64_MAX);
	if (r == VK_TIMEOUT)
		cemuLog_log(LogType::Force, "vkWaitForFences: Returned VK_TIMEOUT on infinite fence");
	else if (r != VK_SUCCESS)
		UnrecoverableError(fmt::format("vkWaitForFences: Returned unhandled error {}", (sint32)r).c_str());
	ProcessFinishedCommandBuffers();
}

void VulkanRenderer::SubmitCommandBuffer(VkSemaphore signalSemaphore, VkSemaphore waitSemaphore)
{
	draw_endRenderPass();
	occlusionQuery_notifyEndCommandBuffer();
	vkEndCommandBuffer(m_state.currentCommandBuffer);

	// submissions are chained: each waits on the previous one's semaphore and signals its own
	VkSemaphore signals[2];
	uint32 signalCount = 0;
	signals[signalCount++] = m_commandBufferSemaphores[m_commandBufferIndex];
	if (signalSemaphore != VK_NULL_HANDLE)
		signals[signalCount++] = signalSemaphore;

	VkSemaphore waits[2];
	uint32 waitCount = 0;
	const VkSemaphore previous = GetLastSubmittedCmdBufferSemaphore();
	if (m_numSubmittedCmdBuffers > 0)
		waits[waitCount++] = previous;
	if (waitSemaphore != VK_NULL_HANDLE)
		waits[waitCount++] = waitSemaphore;
	static const VkPipelineStageFlags waitStages[2] = { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };

	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = waitCount;
	submit.pWaitSemaphores = waits;
	submit.pWaitDstStageMask = waitStages;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_state.currentCommandBuffer;
	submit.signalSemaphoreCount = signalCount;
	submit.pSignalSemaphores = signals;
	const VkResult r = vkQueueSubmit(m_graphicsQueue, 1, &submit, m_cmdBufferFences[m_commandBufferIndex]);
	if (r != VK_SUCCESS)
		UnrecoverableError(fmt::format("failed to submit command buffer. Error {}", r).c_str());
	m_numSubmittedCmdBuffers++;

	ProcessFinishedCommandBuffers();

	// advance to the next ring slot, waiting if the GPU still owns it
	const size_t next = (m_commandBufferIndex + 1) % m_commandBuffers.size();
	if (next == m_commandBufferSyncIndex)
	{
		cemuLog_logDebug(LogType::Force, "Vulkan: Waiting for available command buffer...");
		WaitForNextFinishedCommandBuffer();
	}
	m_cmdBufferUniformRingbufIndices[next] = m_cmdBufferUniformRingbufIndices[m_commandBufferIndex];
	m_commandBufferIndex = next;

	m_state.currentCommandBuffer = m_commandBuffers[next];
	vkResetFences(m_logicalDevice, 1, &m_cmdBufferFences[next]);
	vkResetCommandBuffer(m_state.currentCommandBuffer, 0);
	BeginOneTimeCommandBuffer(m_state.currentCommandBuffer);
	vkCmdSetViewport(m_state.currentCommandBuffer, 0, 1, &m_state.currentViewport);
	vkCmdSetScissor(m_state.currentCommandBuffer, 0, 1, &m_state.currentScissorRect);
	m_state.resetCommandBufferState();
	occlusionQuery_notifyBeginCommandBuffer();

	m_recordedDrawcalls = 0;
	m_submitThreshold = 300;
	m_submitOnIdle = false;
}

void VulkanRenderer::RequestSubmitSoon()
{
	// at most ten more draws go into this command buffer
	m_submitThreshold = std::min(m_submitThreshold, m_recordedDrawcalls + 10);
}

void VulkanRenderer::RequestSubmitOnIdle()
{
	m_submitOnIdle = true;
}

uint64 VulkanRenderer::GetCurrentCommandBufferId() const
{
	return m_numSubmittedCmdBuffers;
}

bool VulkanRenderer::HasCommandBufferFinished(uint64 commandBufferId) const
{
	return commandBufferId < m_countCommandBufferFinished;
}

void VulkanRenderer::WaitCommandBufferFinished(uint64 commandBufferId)
{
	if (commandBufferId == m_numSubmittedCmdBuffers)
		SubmitCommandBuffer(); // the id still refers to the buffer being recorded
	while (!HasCommandBufferFinished(commandBufferId))
		WaitForNextFinishedCommandBuffer();
}

namespace
{
	fs::path DriverPipelineCacheFile(const fs::path& dir)
	{
		return dir / fmt::format("{:016x}.bin", CafeSystem::GetForegroundTitleId());
	}
}

void VulkanRenderer::PipelineCacheSaveThread(size_t cache_size)
{
	SetThreadName("vkDriverPlCache");
	const fs::path dir = ActiveSettings::GetCachePath("shaderCache/driver/vk");
	if (!fs::exists(dir))
	{
		try
		{
			fs::create_directories(dir);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "can't create vulkan pipeline cache directory \"{}\": {}", _pathToUtf8(dir), ex.what());
			return;
		}
	}
	const fs::path file = DriverPipelineCacheFile(dir);
	size_t savedSize = cache_size;

	while (!m_destructionRequested)
	{
		// woken whenever a pipeline was added; then wait 15 seconds so bursts get written once
		m_pipeline_cache_semaphore.wait();
		for (sint32 tick = 0; tick < 60; tick++)
		{
			if (m_destructionRequested)
				return;
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}

		// poll instead of lock(): a waiting exclusive lock would hold off the compiler threads
		while (!m_pipeline_cache_save_mutex.try_lock())
			std::this_thread::sleep_for(std::chrono::milliseconds(250));

		size_t size = 0;
		VkResult r = vkGetPipelineCacheData(m_logicalDevice, m_pipeline_cache, &size, nullptr);
		const bool grown = r == VK_SUCCESS && size > 0 && size != savedSize;
		std::vector<uint8_t> blob;
		if (grown)
		{
			blob.resize(size);
			r = vkGetPipelineCacheData(m_logicalDevice, m_pipeline_cache, &size, blob.data());
		}
		m_pipeline_cache_semaphore.reset();
		m_pipeline_cache_save_mutex.unlock();
		if (!grown)
			continue;

		if (r != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "can't retrieve pipeline cache data: 0x{:x}", r);
			continue;
		}
		std::ofstream out(file, std::ios::out | std::ios::binary);
		if (!out.is_open())
		{
			cemuLog_log(LogType::Force, "can't write pipeline cache to disk");
			continue;
		}
		out.write((const char*)blob.data(), blob.size());
		out.close();
		savedSize = size;
		cemuLog_logDebug(LogType::Force, "pipeline cache saved");
	}
}

void VulkanRenderer::CreatePipelineCache()
{
	// seed the driver cache with what the previous session wrote
	std::vector<uint8_t> seed;
	const fs::path dir = ActiveSettings::GetCachePath("shaderCache/driver/vk");
	if (fs::exists(dir))
	{
		std::ifstream in(DriverPipelineCacheFile(dir), std::ios::in | std::ios::binary | std::ios::ate);
		if (in.is_open())
		{
			seed.resize((size_t)in.tellg());
			in.seekg(0, std::ios::beg);
			in.read((char*)seed.data(), seed.size());
			in.close();
		}
	}

	VkPipelineCacheCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	info.initialDataSize = seed.size();
	info.pInitialData = seed.data();
	VkResult r = vkCreatePipelineCache(m_logicalDevice, &info, nullptr, &m_pipeline_cache);
	if (r != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to open Vulkan pipeline cache: {}", r);
		// the stored data was rejected, start empty
		info.initialDataSize = 0;
		info.pInitialData = nullptr;
		r = vkCreatePipelineCache(m_logicalDevice, &info, nullptr, &m_pipeline_cache);
		if (r != VK_SUCCESS)
			UnrecoverableError(fmt::format("Failed to create new Vulkan pipeline cache: {}", r).c_str());
	}

	size_t initialSize = 0;
	vkGetPipelineCacheData(m_logicalDevice, m_pipeline_cache, &initialSize, nullptr);
	m_pipeline_cache_save_thread = std::thread(&VulkanRenderer::PipelineCacheSaveThread, this, initialSize);
}

void VulkanRenderer::swapchain_createDescriptorSetLayout()
{
	// binding 0: the image being shown, binding 1: the output shader's uniforms
	VkDescriptorSetLayoutBinding bindings[2]{};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 2;
	info.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(m_logicalDevice, &info, nullptr, &m_swapchainDescriptorSetLayout) != VK_SUCCESS)
		UnrecoverableError("failed to create descriptor set layout for swapchain");
}

namespace
{
	using DecoderGetter = TextureDecoder* (*)();
	template<typename T>
	TextureDecoder* DecoderOf()
	{
		return T::getInstance();
	}

	// colour formats whose host representation never depends on device support
	struct DirectColorFormat
	{
		Latte::E_GX2SURFFMT gx2;
		VkFormat vk;
		DecoderGetter decoder;
		bool alternate; // host layout differs from the guest layout
	};
	using GxFmt = Latte::E_GX2SURFFMT;
	const DirectColorFormat s_directColorFormats[] = {
		// four channels
		{ GxFmt::R32_G32_B32_A32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, DecoderOf<TextureDecoder_R32_G32_B32_A32_FLOAT>, false },
		{ GxFmt::R32_G32_B32_A32_UINT, VK_FORMAT_R32G32B32A32_UINT, DecoderOf<TextureDecoder_R32_G32_B32_A32_UINT>, false },
		{ GxFmt::R16_G16_B16_A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, DecoderOf<TextureDecoder_R16_G16_B16_A16_FLOAT>, false },
		{ GxFmt::R16_G16_B16_A16_UINT, VK_FORMAT_R16G16B16A16_UINT, DecoderOf<TextureDecoder_R16_G16_B16_A16_UINT>, false },
		{ GxFmt::R16_G16_B16_A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, DecoderOf<TextureDecoder_R16_G16_B16_A16>, false },
		{ GxFmt::R16_G16_B16_A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, DecoderOf<TextureDecoder_R16_G16_B16_A16>, false },
		{ GxFmt::R8_G8_B8_A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, DecoderOf<TextureDecoder_R8_G8_B8_A8>, false },
		{ GxFmt::R8_G8_B8_A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, DecoderOf<TextureDecoder_R8_G8_B8_A8>, false },
		{ GxFmt::R8_G8_B8_A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, DecoderOf<TextureDecoder_R8_G8_B8_A8>, false },
		{ GxFmt::R8_G8_B8_A8_UINT, VK_FORMAT_R8G8B8A8_UINT, DecoderOf<TextureDecoder_R8_G8_B8_A8>, false },
		{ GxFmt::R8_G8_B8_A8_SINT, VK_FORMAT_R8G8B8A8_SINT, DecoderOf<TextureDecoder_R8_G8_B8_A8>, false },
		// two channels
		{ GxFmt::R32_G32_FLOAT, VK_FORMAT_R32G32_SFLOAT, DecoderOf<TextureDecoder_R32_G32_FLOAT>, false },
		{ GxFmt::R32_G32_UINT, VK_FORMAT_R32G32_UINT, DecoderOf<TextureDecoder_R32_G32_UINT>, false },
		{ GxFmt::R16_G16_UNORM, VK_FORMAT_R16G16_UNORM, DecoderOf<TextureDecoder_R16_G16>, false },
		{ GxFmt::R16_G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, DecoderOf<TextureDecoder_R16_G16_FLOAT>, false },
		{ GxFmt::R8_G8_UNORM, VK_FORMAT_R8G8_UNORM, DecoderOf<TextureDecoder_R8_G8>, false },
		{ GxFmt::R8_G8_SNORM, VK_FORMAT_R8G8_SNORM, DecoderOf<TextureDecoder_R8_G8>, false },
		// one channel
		{ GxFmt::R32_FLOAT, VK_FORMAT_R32_SFLOAT, DecoderOf<TextureDecoder_R32_FLOAT>, false },
		{ GxFmt::R32_UINT, VK_FORMAT_R32_UINT, DecoderOf<TextureDecoder_R32_UINT>, false },
		{ GxFmt::R16_FLOAT, VK_FORMAT_R16_SFLOAT, DecoderOf<TextureDecoder_R16_FLOAT>, false },
		{ GxFmt::R16_UNORM, VK_FORMAT_R16_UNORM, DecoderOf<TextureDecoder_R16_UNORM>, false },
		{ GxFmt::R16_SNORM, VK_FORMAT_R16_SNORM, DecoderOf<TextureDecoder_R16_SNORM>, false },
		{ GxFmt::R16_UINT, VK_FORMAT_R16_UINT, DecoderOf<TextureDecoder_R16_UINT>, false },
		{ GxFmt::R8_UNORM, VK_FORMAT_R8_UNORM, DecoderOf<TextureDecoder_R8>, false },
		{ GxFmt::R8_SNORM, VK_FORMAT_R8_SNORM, DecoderOf<TextureDecoder_R8>, false },
		{ GxFmt::R8_UINT, VK_FORMAT_R8_UINT, DecoderOf<TextureDecoder_R8_UINT>, false },
		// packed
		{ GxFmt::R11_G11_B10_FLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32, DecoderOf<TextureDecoder_R11_G11_B10_FLOAT>, false },
		{ GxFmt::R10_G10_B10_A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, DecoderOf<TextureDecoder_R10_G10_B10_A2_UNORM>, false },
		// the packed SNORM variant is not usable, widen to 16 bit per channel
		{ GxFmt::R10_G10_B10_A2_SNORM, VK_FORMAT_R16G16B16A16_SNORM, DecoderOf<TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16>, true },
		// no host sRGB format has more than 8 bits per channel, keep the data linear
		{ GxFmt::R10_G10_B10_A2_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32, DecoderOf<TextureDecoder_R10_G10_B10_A2_UNORM>, true },
		// block compressed
		{ GxFmt::BC1_SRGB, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, DecoderOf<TextureDecoder_BC1>, false },
		{ GxFmt::BC1_UNORM, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, DecoderOf<TextureDecoder_BC1>, false },
		{ GxFmt::BC2_UNORM, VK_FORMAT_BC2_UNORM_BLOCK, DecoderOf<TextureDecoder_BC2>, false },
		{ GxFmt::BC2_SRGB, VK_FORMAT_BC2_SRGB_BLOCK, DecoderOf<TextureDecoder_BC2>, false },
		{ GxFmt::BC3_UNORM, VK_FORMAT_BC3_UNORM_BLOCK, DecoderOf<TextureDecoder_BC3>, false },
		{ GxFmt::BC3_SRGB, VK_FORMAT_BC3_SRGB_BLOCK, DecoderOf<TextureDecoder_BC3>, false },
		{ GxFmt::BC4_UNORM, VK_FORMAT_BC4_UNORM_BLOCK, DecoderOf<TextureDecoder_BC4>, false },
		{ GxFmt::BC4_SNORM, VK_FORMAT_BC4_SNORM_BLOCK, DecoderOf<TextureDecoder_BC4>, false },
		{ GxFmt::BC5_UNORM, VK_FORMAT_BC5_UNORM_BLOCK, DecoderOf<TextureDecoder_BC5>, false },
		{ GxFmt::BC5_SNORM, VK_FORMAT_BC5_SNORM_BLOCK, DecoderOf<TextureDecoder_BC5>, false },
		// depth-like colour formats
		{ GxFmt::R24_X8_UNORM, VK_FORMAT_R32_SFLOAT, DecoderOf<TextureDecoder_R24_X8>, true },
		{ GxFmt::X24_G8_UINT, VK_FORMAT_R8G8B8A8_UINT, DecoderOf<TextureDecoder_X24_G8_UINT>, true },
		{ GxFmt::R32_X8_FLOAT, VK_FORMAT_R32_SFLOAT, DecoderOf<TextureDecoder_NullData64>, true },
	};
}

void VulkanRenderer::GetTextureFormatInfoVK(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, sint32 width, sint32 height, FormatInfoVK* formatInfoOut)
{
	FormatInfoVK& out = *formatInfoOut;
	out.texelCountX = width;
	out.texelCountY = height;
	out.isCompressed = false;
	out.isAlternateFormat = false;

	auto set = [&out](VkFormat vk, TextureDecoder* decoder, bool alternate) {
		out.vkImageFormat = vk;
		out.decoder = decoder;
		out.isAlternateFormat = alternate;
	};

	if (isDepth)
	{
		constexpr VkImageAspectFlags depthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		switch (format)
		{
		case GxFmt::D24_S8_UNORM:
			out.vkImageAspect = depthStencil;
			if (m_supportedFormatInfo.fmt_d24_unorm_s8_uint)
				set(VK_FORMAT_D24_UNORM_S8_UINT, TextureDecoder_D24_S8::getInstance(), false);
			else
				set(VK_FORMAT_D32_SFLOAT_S8_UINT, TextureDecoder_NullData64::getInstance(), true);
			break;
		case GxFmt::D24_S8_FLOAT:
			out.vkImageAspect = depthStencil;
			set(VK_FORMAT_D32_SFLOAT_S8_UINT, TextureDecoder_NullData64::getInstance(), true);
			break;
		case GxFmt::D32_FLOAT:
			out.vkImageAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			set(VK_FORMAT_D32_SFLOAT, TextureDecoder_R32_FLOAT::getInstance(), false);
			break;
		case GxFmt::D16_UNORM:
			out.vkImageAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			set(VK_FORMAT_D16_UNORM, TextureDecoder_R16_UNORM::getInstance(), false);
			break;
		case GxFmt::D32_S8_FLOAT:
			out.vkImageAspect = depthStencil;
			set(VK_FORMAT_D32_SFLOAT_S8_UINT, TextureDecoder_D32_S8_UINT_X24::getInstance(), false);
			break;
		default:
			cemuLog_log(LogType::Force, "Unsupported depth texture format {:04x}", (uint32)format);
			out.vkImageAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			set(VK_FORMAT_D16_UNORM, nullptr, true); // placeholder
			break;
		}
		return;
	}

	out.vkImageAspect = VK_IMAGE_ASPECT_COLOR_BIT;
	// an sRGB flag on a float format means nothing (seen in Sonic Lost World's Starry Speedway level)
	if (format == (GxFmt::R16_G16_B16_A16_FLOAT | GxFmt::FMT_BIT_SRGB))
		format = GxFmt::R16_G16_B16_A16_FLOAT;

	const auto& sf = m_supportedFormatInfo;
	switch (format)
	{
	// small packed formats: use the matching packed host format when the device has it,
	// otherwise expand to RGBA8. Channel order differs from GX2 either way.
	case GxFmt::R4_G4_UNORM:
		if (sf.fmt_r4g4_unorm_pack)
			set(VK_FORMAT_R4G4_UNORM_PACK8, TextureDecoder_R4_G4::getInstance(), true);
		else if (sf.fmt_r4g4b4a4_unorm_pack)
			set(VK_FORMAT_R4G4B4A4_UNORM_PACK16, TextureDecoder_R4_G4_UNORM_To_ABGR4::getInstance(), true);
		else
			set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R4G4_UNORM_To_RGBA8::getInstance(), true);
		return;
	case GxFmt::R5_G6_B5_UNORM:
		if (sf.fmt_r5g6b5_unorm_pack)
			set(VK_FORMAT_R5G6B5_UNORM_PACK16, TextureDecoder_R5_G6_B5_swappedRB::getInstance(), true);
		else
			set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R5G6B5_UNORM_To_RGBA8::getInstance(), true);
		return;
	case GxFmt::R5_G5_B5_A1_UNORM:
		if (sf.fmt_a1r5g5b5_unorm_pack)
			set(VK_FORMAT_A1R5G5B5_UNORM_PACK16, TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB::getInstance(), true);
		else
			set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8::getInstance(), true);
		return;
	case GxFmt::A1_B5_G5_R5_UNORM:
		if (sf.fmt_a1r5g5b5_unorm_pack)
			set(VK_FORMAT_A1R5G5B5_UNORM_PACK16, TextureDecoder_A1_B5_G5_R5_UNORM_vulkan::getInstance(), true);
		else
			set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8::getInstance(), true);
		return;
	case GxFmt::R4_G4_B4_A4_UNORM:
		if (sf.fmt_r4g4b4a4_unorm_pack)
			set(VK_FORMAT_R4G4B4A4_UNORM_PACK16, TextureDecoder_R4_G4_B4_A4_UNORM::getInstance(), true);
		else
			set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R4G4B4A4_UNORM_To_RGBA8::getInstance(), true);
		return;
	default:
		break;
	}

	for (const DirectColorFormat& entry : s_directColorFormats)
	{
		if (entry.gx2 == format)
		{
			set(entry.vk, entry.decoder(), entry.alternate);
			return;
		}
	}
	cemuLog_log(LogType::Force, "Unsupported color texture format {:04x}", (uint32)format);
	cemu_assert_debug(false);
	set(VK_FORMAT_UNDEFINED, nullptr, false);
}

VkPipelineShaderStageCreateInfo VulkanRenderer::CreatePipelineShaderStageCreateInfo(VkShaderStageFlagBits stage, VkShaderModule& module, const char* entryName) const
{
	VkPipelineShaderStageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage = stage;
	info.module = module;
	info.pName = entryName;
	return info;
}

VkPipeline VulkanRenderer::backbufferBlit_createGraphicsPipeline(VkDescriptorSetLayout descriptorLayout, bool padView, RendererOutputShader* shader)
{
	auto& chain = GetChainInfo(!padView);
	RendererShaderVk* vs = static_cast<RendererShaderVk*>(shader->GetVertexShader());
	RendererShaderVk* fs = static_cast<RendererShaderVk*>(shader->GetFragmentShader());

	// one pipeline per shader pair and target window
	const uint64 key = (uint64)(uintptr_t)vs * 0x9E3779B97F4A7C15ull ^ ((uint64)(uintptr_t)fs + (padView ? 1ull : 0ull));
	if (const auto it = m_backbufferBlitPipelineCache.find(key); it != m_backbufferBlitPipelineCache.end())
		return it->second;

	std::vector<VkPipelineShaderStageCreateInfo> stages;
	if (vs)
		stages.push_back(CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, vs->GetShaderModule(), "main"));
	if (fs)
		stages.push_back(CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, fs->GetShaderModule(), "main"));

	// a full screen triangle pair generated in the vertex shader, no vertex input
	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo assembly{};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{};
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	const VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicInfo{};
	dynamicInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicInfo.dynamicStateCount = 2;
	dynamicInfo.pDynamicStates = dynamic;
	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_BACK_BIT;
	raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState blendAttachment{};
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blend{};
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.logicOp = VK_LOGIC_OP_COPY;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;

	// the layout is shared by all blit pipelines
	if (m_pipelineLayout == VK_NULL_HANDLE)
	{
		VkPipelineLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &descriptorLayout;
		const VkResult r = vkCreatePipelineLayout(m_logicalDevice, &layoutInfo, nullptr, &m_pipelineLayout);
		if (r != VK_SUCCESS)
			throw std::runtime_error(fmt::format("Failed to create pipeline layout: {}", r));
	}

	VkGraphicsPipelineCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = (uint32)stages.size();
	info.pStages = stages.data();
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamicInfo;
	info.layout = m_pipelineLayout;
	info.renderPass = chain.m_swapchainRenderPass;
	info.subpass = 0;

	VkPipeline pipeline = nullptr;
	std::shared_lock cacheLock(m_pipeline_cache_save_mutex);
	const VkResult r = vkCreateGraphicsPipelines(m_logicalDevice, m_pipeline_cache, 1, &info, nullptr, &pipeline);
	if (r != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create graphics pipeline. Error {}", r);
		throw std::runtime_error(fmt::format("Failed to create graphics pipeline: {}", r));
	}
	m_backbufferBlitPipelineCache[key] = pipeline;
	m_pipeline_cache_semaphore.notify();
	return pipeline;
}

bool VulkanRenderer::AcquireNextSwapchainImage(bool mainWindow)
{
	if (!IsSwapchainInfoValid(mainWindow))
		return false;

	// the UI thread asked for the pad swapchain to go away (StopUsingPadAndWait)
	if (!mainWindow && m_destroyPadSwapchainNextAcquire.test())
	{
		RecreateSwapchain(mainWindow, true);
		m_destroyPadSwapchainNextAcquire.clear();
		m_destroyPadSwapchainNextAcquire.notify_all();
		return false;
	}

	auto& chain = GetChainInfo(mainWindow);
	if (chain.swapchainImageIndex != -1)
		return true; // this frame already holds an image
	if (!UpdateSwapchainProperties(mainWindow))
		return false;
	if (!chain.AcquireImage())
		return false;
	// later work on the image must wait for the acquire
	SubmitCommandBuffer(VK_NULL_HANDLE, chain.ConsumeAcquireSemaphore());
	return true;
}

void VulkanRenderer::RecreateSwapchain(bool mainWindow, bool skipCreate)
{
	SubmitCommandBuffer();
	WaitDeviceIdle();
	auto& chain = GetChainInfo(mainWindow);

	Vector2i size;
	if (mainWindow)
	{
		ImGui_ImplVulkan_Shutdown(); // imgui holds the old render pass
		WindowSystem::GetWindowPhysSize(size.x, size.y);
	}
	else
		WindowSystem::GetPadWindowPhysSize(size.x, size.y);

	chain.swapchainImageIndex = -1;
	chain.Cleanup();
	chain.m_desiredExtent = size;
	if (!skipCreate)
		chain.Create();
	if (mainWindow)
		ImguiInit();
}

bool VulkanRenderer::UpdateSwapchainProperties(bool mainWindow)
{
	auto& chain = GetChainInfo(mainWindow);
	const auto vsync = (VSync)GetConfig().vsync.GetValue();

	int width, height;
	if (mainWindow)
		WindowSystem::GetWindowPhysSize(width, height);
	else
		WindowSystem::GetPadWindowPhysSize(width, height);
	const VkExtent2D extent = chain.getExtent();

	const bool recreate = chain.m_shouldRecreate || chain.m_vsyncState != vsync || width != extent.width || height != extent.height;
	if (recreate)
	{
		try
		{
			RecreateSwapchain(mainWindow);
		}
		catch (std::exception&)
		{
			cemu_assert_debug(false);
			return false;
		}
	}
	chain.m_shouldRecreate = false;
	chain.m_vsyncState = vsync;
	return true;
}

void VulkanRenderer::SwapBuffer(bool mainWindow)
{
	if (!AcquireNextSwapchainImage(mainWindow))
		return;
	auto& chain = GetChainInfo(mainWindow);

	if (!chain.hasDefinedSwapchainImage)
	{
		// nothing was drawn this frame, present black instead of undefined contents
		const VkClearColorValue black{};
		ClearColorImageRaw(chain.m_swapchainImages[chain.swapchainImageIndex], 0, 0, black, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}

	const size_t frameCmdBufferId = GetCurrentCommandBufferId();
	VkSemaphore readyToPresent = chain.m_presentSemaphores[chain.swapchainImageIndex];
	SubmitCommandBuffer(readyToPresent);
	cemu_assert_debug(m_numSubmittedCmdBuffers > 0);

	// keep at most one frame in flight on the CPU side
	WaitCommandBufferFinished(m_commandBufferIDOfPrevFrame);
	m_commandBufferIDOfPrevFrame = frameCmdBufferId;
	chain.WaitAvailableFence();

	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &readyToPresent;
	present.swapchainCount = 1;
	present.pSwapchains = &chain.m_swapchain;
	present.pImageIndices = &chain.swapchainImageIndex;

	// with present_wait, tag each present and bound the number queued in the swapchain
	VkPresentIdKHR presentId{};
	if (m_featureControl.deviceExtensions.present_wait && chain.m_maxQueued > 0)
	{
		presentId.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
		presentId.swapchainCount = 1;
		presentId.pPresentIds = &chain.m_presentId;
		present.pNext = &presentId;
		if (chain.m_queueDepth >= chain.m_maxQueued)
		{
			const uint64 oldestQueued = chain.m_presentId - chain.m_queueDepth;
			vkWaitForPresentKHR(m_logicalDevice, chain.m_swapchain, oldestQueued, 40'000'000);
			chain.m_queueDepth--;
		}
	}

	const VkResult r = vkQueuePresentKHR(m_presentQueue, &present);
	if (r < 0 && r != VK_ERROR_OUT_OF_DATE_KHR)
		throw std::runtime_error(fmt::format("Failed to present image: {}", r));
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
		chain.m_shouldRecreate = true;
	if (r >= 0)
	{
		chain.m_queueDepth++;
		chain.m_presentId++;
	}
	chain.hasDefinedSwapchainImage = false;
	chain.swapchainImageIndex = -1;
}

void VulkanRenderer::Flush(bool waitIdle)
{
	if (m_recordedDrawcalls > 0 || m_submitOnIdle)
		SubmitCommandBuffer();
	if (waitIdle)
		WaitCommandBufferFinished(GetCurrentCommandBufferId());
}

void VulkanRenderer::NotifyLatteCommandProcessorIdle()
{
	if (m_submitOnIdle)
		SubmitCommandBuffer();
}

void VulkanBenchmarkPrintResults(); // VulkanAPI.cpp

void VulkanRenderer::SwapBuffers(bool swapTV, bool swapDRC)
{
	SubmitCommandBuffer();
	if (swapTV && IsSwapchainInfoValid(true))
		SwapBuffer(true);
	if (swapDRC && IsSwapchainInfoValid(false))
		SwapBuffer(false);
	if (swapTV)
		VulkanBenchmarkPrintResults();
}

void VulkanRenderer::ClearColorbuffer(bool padView)
{
	if (!IsSwapchainInfoValid(!padView))
		return;
	auto& chain = GetChainInfo(!padView);
	if (chain.swapchainImageIndex == -1)
		return;
	const VkClearColorValue black{};
	ClearColorImageRaw(chain.m_swapchainImages[chain.swapchainImageIndex], 0, 0, black, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
}

void VulkanRenderer::ClearColorImageRaw(VkImage image, uint32 sliceIndex, uint32 mipIndex, const VkClearColorValue& color, VkImageLayout inputLayout, VkImageLayout outputLayout)
{
	draw_endRenderPass();
	const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, mipIndex, 1, sliceIndex, 1 };
	barrier_image<ANY_TRANSFER | IMAGE_READ | IMAGE_WRITE, ANY_TRANSFER>(image, range, inputLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdClearColorImage(m_state.currentCommandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
	barrier_image<ANY_TRANSFER, ANY_TRANSFER | IMAGE_READ | IMAGE_WRITE>(image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, outputLayout);
}

void VulkanRenderer::ClearColorImage(LatteTextureVk* vkTexture, uint32 sliceIndex, uint32 mipIndex, const VkClearColorValue& color, VkImageLayout outputLayout)
{
	if (vkTexture->isDepth)
	{
		cemu_assert_suspicious();
		return;
	}
	VKRObjectTexture* imageObj = vkTexture->GetImageObj();
	imageObj->flagForCurrentCommandBuffer();

	if (vkTexture->IsCompressedFormat())
	{
		// vkCmdClearColorImage does not accept block compressed images. The clear is dropped,
		// but the layout transition still happens so later users find the expected layout
		const VkImageSubresourceLayers layer{ VK_IMAGE_ASPECT_COLOR_BIT, mipIndex, sliceIndex, 1 };
		barrier_image<ANY_TRANSFER | IMAGE_READ, ANY_TRANSFER | IMAGE_READ | IMAGE_WRITE>(vkTexture, layer, outputLayout);
		const bool toZero = color.float32[0] == 0.0f && color.float32[1] == 0.0f && color.float32[2] == 0.0f && color.float32[3] == 0.0f;
		static bool reported = false;
		if (toZero && !reported)
		{
			cemuLog_logDebug(LogType::Force, "Unsupported compressed texture clear to zero");
			reported = true;
		}
		return;
	}

	VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, mipIndex, 1, sliceIndex, 1 };
	const VkImageLayout currentLayout = vkTexture->GetImageLayout(range);
	ClearColorImageRaw(imageObj->m_image, sliceIndex, mipIndex, color, currentLayout, outputLayout);
	vkTexture->SetImageLayout(range, outputLayout);
}

void VulkanRenderer::DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter, sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground)
{
	if (!AcquireNextSwapchainImage(!padView))
		return;
	auto& chain = GetChainInfo(!padView);
	LatteTextureViewVk* viewVk = (LatteTextureViewVk*)texView;
	draw_endRenderPass();

	// the source was last written as attachment or by a transfer, the blit samples it
	VkMemoryBarrier sourceReady{};
	sourceReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	sourceReady.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	sourceReady.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(m_state.currentCommandBuffer,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 1, &sourceReady, 0, nullptr, 0, nullptr);

	const VkPipeline pipeline = backbufferBlit_createGraphicsPipeline(m_swapchainDescriptorSetLayout, padView, shader);
	const VkExtent2D extent = chain.getExtent();

	// the image rectangle inside the window; the scissor covers the whole window
	VkViewport viewport{};
	viewport.x = imageX;
	viewport.y = imageY;
	viewport.width = imageWidth;
	viewport.height = imageHeight;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(m_state.currentCommandBuffer, 0, 1, &viewport);
	VkRect2D scissor{};
	scissor.extent = extent;
	vkCmdSetScissor(m_state.currentCommandBuffer, 0, 1, &scissor);

	VkDescriptorSet descriptorSet = backbufferBlit_createDescriptorSet(m_swapchainDescriptorSetLayout, viewVk, useLinearTexFilter);

	VkRenderPassBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = chain.m_swapchainRenderPass;
	begin.framebuffer = chain.m_swapchainFramebuffers[chain.swapchainImageIndex];
	begin.renderArea.extent = extent;
	vkCmdBeginRenderPass(m_state.currentCommandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);

	if (clearBackground)
	{
		VkClearAttachment clear{};
		clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		clear.colorAttachment = 0;
		clear.clearValue = {};
		VkClearRect rect{};
		rect.rect.extent = chain.m_actualExtent;
		rect.baseArrayLayer = 0;
		rect.layerCount = 1;
		vkCmdClearAttachments(m_state.currentCommandBuffer, 1, &clear, 1, &rect);
	}

	vkCmdBindPipeline(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	m_state.currentPipeline = pipeline;

	auto uniforms = shader->FillUniformBlockBuffer(*texView, { imageWidth, imageHeight }, padView);
	const uint32 uniformOffset = uniformData_uploadUniformDataBufferGetOffset({ (uint8*)&uniforms, sizeof(uniforms) });
	vkCmdBindDescriptorSets(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &descriptorSet, 1, &uniformOffset);
	vkCmdDraw(m_state.currentCommandBuffer, 6, 1, 0, 0);
	vkCmdEndRenderPass(m_state.currentCommandBuffer);

	vkCmdSetViewport(m_state.currentCommandBuffer, 0, 1, &m_state.currentViewport);
	chain.hasDefinedSwapchainImage = true;
}

void VulkanRenderer::CreateDescriptorPool()
{
	// one shared pool; sets are freed individually
	const VkDescriptorPoolSize sizes[] = {
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 * 1024 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * 1024 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 128 * 1024 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * 1024 },
	};
	VkDescriptorPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	info.maxSets = 256 * 1024;
	info.poolSizeCount = (uint32)std::size(sizes);
	info.pPoolSizes = sizes;
	if (vkCreateDescriptorPool(m_logicalDevice, &info, nullptr, &m_descriptorPool) != VK_SUCCESS)
		UnrecoverableError("Failed to create descriptor pool!");
}

VkDescriptorSet VulkanRenderer::backbufferBlit_createDescriptorSet(VkDescriptorSetLayout descriptor_set_layout, LatteTextureViewVk* texViewVk, bool useLinearTexFilter)
{
	VKRObjectTextureView* view = texViewVk->GetViewRGBA();
	const VkSampler sampler = texViewVk->GetDefaultTextureSampler(useLinearTexFilter);
	// one set per view and sampler combination, kept until shutdown
	const uint64 key = (uint64)(uintptr_t)view * 0x9E3779B97F4A7C15ull ^ (uint64)(uintptr_t)sampler;
	if (const auto it = m_backbufferBlitDescriptorSetCache.find(key); it != m_backbufferBlitDescriptorSetCache.end())
		return it->second;

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &descriptor_set_layout;
	VkDescriptorSet set;
	if (vkAllocateDescriptorSets(m_logicalDevice, &allocInfo, &set) != VK_SUCCESS)
		UnrecoverableError("Failed to allocate descriptor sets for backbuffer blit");
	performanceMonitor.vk.numDescriptorSets.increment();

	VkDescriptorImageInfo image{};
	image.sampler = sampler;
	image.imageView = view->m_textureImageView;
	image.imageLayout = static_cast<LatteTextureVk*>(texViewVk->baseTexture)->GetDefaultLayout();
	VkDescriptorBufferInfo uniforms{};
	uniforms.buffer = m_uniformVarBuffer;
	uniforms.offset = 0; // the real offset is passed as dynamic offset at bind time
	uniforms.range = sizeof(RendererOutputShader::OutputUniformVariables);

	VkWriteDescriptorSet writes[2]{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].pImageInfo = &image;
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = set;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	writes[1].pBufferInfo = &uniforms;
	vkUpdateDescriptorSets(m_logicalDevice, 2, writes, 0, nullptr);
	performanceMonitor.vk.numDescriptorSamplerTextures.increment();

	m_backbufferBlitDescriptorSetCache[key] = set;
	return set;
}

void VulkanRenderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
	// halfZ is handled in the vertex shader. The viewport is flipped vertically (negative height)
	VkViewport flipped{};
	flipped.x = x;
	flipped.y = y + height;
	flipped.width = width;
	flipped.height = -height;
	flipped.minDepth = nearZ;
	flipped.maxDepth = farZ;

	VkViewport& cur = m_state.currentViewport;
	const bool same = cur.x == flipped.x && cur.y == flipped.y && cur.width == flipped.width && cur.height == flipped.height &&
		cur.minDepth == flipped.minDepth && cur.maxDepth == flipped.maxDepth;
	if (same)
		return;
	cur = flipped;
	vkCmdSetViewport(m_state.currentCommandBuffer, 0, 1, &cur);
}

void VulkanRenderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
	VkRect2D& rect = m_state.currentScissorRect;
	rect.offset = { scissorX, scissorY };
	rect.extent = { (uint32)scissorWidth, (uint32)scissorHeight };
	vkCmdSetScissor(m_state.currentCommandBuffer, 0, 1, &rect);
}

LatteCachedFBO* VulkanRenderer::rendertarget_createCachedFBO(uint64 key)
{
	return new CachedFBOVk(key, m_logicalDevice);
}

void VulkanRenderer::rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo)
{
	if (m_state.activeFBO == cfbo)
		m_state.activeFBO = nullptr;
}

void VulkanRenderer::rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo)
{
	m_state.activeFBO = (CachedFBOVk*)cfbo;
}

void* VulkanRenderer::texture_acquireTextureUploadBuffer(uint32 size)
{
	return memoryManager->TextureUploadBufferAcquire(size);
}

void VulkanRenderer::texture_releaseTextureUploadBuffer(uint8* mem)
{
	memoryManager->TextureUploadBufferRelease(mem);
}

TextureDecoder* VulkanRenderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
	FormatInfoVK info{};
	GetTextureFormatInfoVK(format, isDepth, dim, width, height, &info);
	return info.decoder;
}

void VulkanRenderer::ReleaseDestructibleObject(VKRDestructibleObject* destructibleObject)
{
	if (destructibleObject->canDestroy())
	{
		delete destructibleObject;
		return;
	}
	// still referenced or used by an unfinished command buffer
	m_spinlockDestructionQueue.lock();
	m_destructionQueue.emplace_back(destructibleObject);
	m_spinlockDestructionQueue.unlock();
}

void VulkanRenderer::ProcessDestructionQueue()
{
	m_spinlockDestructionQueue.lock();
	// delete what can go, keep the rest in order
	auto keep = m_destructionQueue.begin();
	for (auto it = m_destructionQueue.begin(); it != m_destructionQueue.end(); ++it)
	{
		if ((*it)->canDestroy())
			delete *it;
		else
			*keep++ = *it;
	}
	m_destructionQueue.erase(keep, m_destructionQueue.end());
	m_spinlockDestructionQueue.unlock();
}

VkDescriptorSetInfo::~VkDescriptorSetInfo()
{
	for (auto& view : list_referencedViews)
		view->RemoveDescriptorSetReference(this);
	const auto removed = pipeline_info->GetDescriptorSetCache(shaderType).erase(stateHash);
	cemu_assert_debug(removed == 1);
	performanceMonitor.vk.numDescriptorSamplerTextures.decrement(statsNumSamplerTextures);
	performanceMonitor.vk.numDescriptorDynUniformBuffers.decrement(statsNumDynUniformBuffers);
	performanceMonitor.vk.numDescriptorStorageBuffers.decrement(statsNumStorageBuffers);
	VulkanRenderer::GetInstance()->ReleaseDestructibleObject(m_vkObjDescriptorSet);
	m_vkObjDescriptorSet = nullptr;
}

void VulkanRenderer::texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex)
{
	draw_endRenderPass();
	auto vkTexture = (LatteTextureVk*)hostTexture;
	if (vkTexture->isDepth)
	{
		texture_clearDepthSlice(hostTexture, sliceIndex, mipIndex, true, vkTexture->hasStencil, 0.0f, 0);
		return;
	}
	cemu_assert_debug(vkTexture->dim != Latte::E_DIM::DIM_3D);
	ClearColorImage(vkTexture, sliceIndex, mipIndex, { 0, 0, 0, 0 }, vkTexture->GetDefaultLayout());
}

void VulkanRenderer::texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
	auto vkTexture = (LatteTextureVk*)hostTexture;
	if (vkTexture->dim == Latte::E_DIM::DIM_3D)
		cemu_assert_unimplemented();
	ClearColorImage(vkTexture, sliceIndex, mipIndex, { r, g, b, a }, vkTexture->GetDefaultLayout());
}

void VulkanRenderer::texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
	draw_endRenderPass(); // depth/stencil image clears are not allowed inside a render pass
	auto vkTexture = (LatteTextureVk*)hostTexture;
	const VkImageAspectFlags available = vkTexture->GetImageAspect();
	VkImageAspectFlags cleared = 0;
	if (clearDepth)
		cleared |= available & VK_IMAGE_ASPECT_DEPTH_BIT;
	if (clearStencil)
		cleared |= available & VK_IMAGE_ASPECT_STENCIL_BIT;

	VKRObjectTexture* imageObj = vkTexture->GetImageObj();
	imageObj->flagForCurrentCommandBuffer();

	// the layout transition covers every aspect of the slice
	const VkImageSubresourceLayers layer{ available, (uint32)mipIndex, sliceIndex, 1 };
	barrier_image<ANY_TRANSFER | IMAGE_READ | IMAGE_WRITE, ANY_TRANSFER>(vkTexture, layer, VK_IMAGE_LAYOUT_GENERAL);

	VkClearDepthStencilValue value{};
	value.depth = depthValue;
	value.stencil = stencilValue;
	const VkImageSubresourceRange range{ cleared, (uint32)mipIndex, 1, sliceIndex, 1 };
	vkCmdClearDepthStencilImage(m_state.currentCommandBuffer, imageObj->m_image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);

	barrier_image<ANY_TRANSFER, ANY_TRANSFER | IMAGE_READ | IMAGE_WRITE>(vkTexture, layer, vkTexture->GetDefaultLayout());
}

void VulkanRenderer::texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
	auto vkTexture = (LatteTextureVk*)hostTexture;
	VKRObjectTexture* imageObj = vkTexture->GetImageObj();
	imageObj->flagForCurrentCommandBuffer();
	draw_endRenderPass();

	// stage the decoded texels, aligned like the image itself
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(m_logicalDevice, imageObj->m_image, &req);
	VKRSynchronizedRingAllocator& staging = memoryManager->getStagingAllocator();
	auto upload = staging.AllocateBufferMemory(compressedImageSize, (uint32)req.alignment);
	memcpy(upload.memPtr, pixelData, compressedImageSize);
	staging.FlushReservation(upload);

	FormatInfoVK fmt{};
	GetTextureFormatInfoVK(hostTexture->format, hostTexture->isDepth, hostTexture->dim, 0, 0, &fmt);
	const bool volume = hostTexture->Is3DTexture();
	// a 3D texture keeps its slices along z within layer 0
	const uint32 layer = volume ? 0 : (uint32)sliceIndex;

	const VkImageSubresourceLayers barrierLayer{ fmt.vkImageAspect, (uint32)mipIndex, layer, 1 };
	barrier_image<ANY_TRANSFER | IMAGE_READ | IMAGE_WRITE | HOST_WRITE, ANY_TRANSFER>(vkTexture, barrierLayer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	auto regionFor = [&](VkImageAspectFlags aspect, uint32 arrayLayer, sint32 zOffset) {
		VkBufferImageCopy region{};
		region.bufferOffset = upload.bufferOffset;
		region.imageSubresource = { aspect, (uint32)mipIndex, arrayLayer, 1 };
		region.imageOffset = { 0, 0, zOffset };
		region.imageExtent = { (uint32)width, (uint32)height, 1 };
		return region;
	};
	VkBufferImageCopy regions[2];
	uint32 regionCount = 0;
	if (fmt.vkImageAspect == VK_IMAGE_ASPECT_COLOR_BIT || fmt.vkImageAspect == VK_IMAGE_ASPECT_DEPTH_BIT)
		regions[regionCount++] = regionFor(fmt.vkImageAspect, layer, volume ? sliceIndex : 0);
	else if (fmt.vkImageAspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
	{
		// depth and stencil are separate copies reading the same staged data
		cemu_assert_debug(!volume);
		regions[regionCount++] = regionFor(VK_IMAGE_ASPECT_DEPTH_BIT, (uint32)sliceIndex, 0);
		regions[regionCount++] = regionFor(VK_IMAGE_ASPECT_STENCIL_BIT, (uint32)sliceIndex, 0);
	}
	else
		cemu_assert_debug(false);
	vkCmdCopyBufferToImage(m_state.currentCommandBuffer, upload.vkBuffer, imageObj->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regionCount, regions);

	barrier_image<ANY_TRANSFER, ANY_TRANSFER | IMAGE_READ | IMAGE_WRITE>(vkTexture, barrierLayer, vkTexture->GetDefaultLayout());
}

LatteTexture* VulkanRenderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
	uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	return new LatteTextureVk(this, dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
}

void VulkanRenderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit)
{
	m_state.boundTexture[textureUnit] = static_cast<LatteTextureViewVk*>(textureView);
}

void VulkanRenderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth)
{
	LatteTextureVk* srcVk = static_cast<LatteTextureVk*>(src);
	LatteTextureVk* dstVk = static_cast<LatteTextureVk*>(dst);
	draw_endRenderPass(); // image copies must be recorded outside a render pass

	VKRObjectTexture* srcObj = srcVk->GetImageObj();
	VKRObjectTexture* dstObj = dstVk->GetImageObj();
	srcObj->flagForCurrentCommandBuffer();
	dstObj->flagForCurrentCommandBuffer();

	// srcDepth counts z slices for 3D textures and array layers otherwise
	VkImageCopy region{};
	region.srcSubresource.aspectMask = srcVk->GetImageAspect();
	region.srcSubresource.mipLevel = srcMip;
	region.dstSubresource.aspectMask = dstVk->GetImageAspect();
	region.dstSubresource.mipLevel = dstMip;
	region.srcOffset = { effectiveSrcX, effectiveSrcY, 0 };
	region.dstOffset = { effectiveDstX, effectiveDstY, 0 };
	region.extent = { (uint32)effectiveCopyWidth, (uint32)effectiveCopyHeight, 1 };
	if (src->Is3DTexture())
	{
		region.srcOffset.z = srcSlice;
		region.extent.depth = srcDepth;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount = 1;
	}
	else
	{
		region.srcSubresource.baseArrayLayer = srcSlice;
		region.srcSubresource.layerCount = srcDepth;
	}
	if (dst->Is3DTexture())
	{
		region.dstOffset.z = dstSlice;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount = 1;
	}
	else
	{
		region.dstSubresource.baseArrayLayer = dstSlice;
		region.dstSubresource.layerCount = srcDepth;
	}

	// an uncompressed source cannot fill a compressed mip smaller than one 4x4 block
	if (!Latte::IsCompressedFormat(srcVk->format) && Latte::IsCompressedFormat(dstVk->format))
	{
		const sint32 mipWidth = std::max(dst->width >> dstMip, 1);
		const sint32 mipHeight = std::max(dst->height >> dstMip, 1);
		if (mipWidth < 4 || mipHeight < 4)
		{
			cemuLog_logDebug(LogType::Force, "vkCmdCopyImage - blocked copy for unsupported uncompressed->compressed copy with dst smaller than 4x4");
			return;
		}
	}

	barrier_image<IMAGE_WRITE | ANY_TRANSFER, ANY_TRANSFER>(srcVk, region.srcSubresource, VK_IMAGE_LAYOUT_GENERAL);
	barrier_image<IMAGE_READ | IMAGE_WRITE | ANY_TRANSFER, ANY_TRANSFER>(dstVk, region.dstSubresource, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdCopyImage(m_state.currentCommandBuffer, srcObj->m_image, VK_IMAGE_LAYOUT_GENERAL, dstObj->m_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
	barrier_image<ANY_TRANSFER, IMAGE_READ | IMAGE_WRITE | ANY_TRANSFER>(srcVk, region.srcSubresource, srcVk->GetDefaultLayout());
	barrier_image<ANY_TRANSFER, IMAGE_READ | IMAGE_WRITE | ANY_TRANSFER>(dstVk, region.dstSubresource, dstVk->GetDefaultLayout());
}

LatteTextureReadbackInfo* VulkanRenderer::texture_createReadback(LatteTextureView* textureView)
{
	auto* readback = new LatteTextureReadbackInfoVk(m_logicalDevice, textureView);
	const uint32 bytes = readback->GetImageSize();
	if (bytes == 0)
	{
		delete readback;
		return nullptr;
	}
	// carve a 256 byte aligned range out of the readback ring, wrapping when it would not fit
	constexpr uint32 alignment = 256;
	uint32 offset = (m_textureReadbackBufferWriteIndex + alignment - 1) & ~(alignment - 1);
	if (offset + bytes + 256 > TEXTURE_READBACK_SIZE)
		offset = 0;
	m_textureReadbackBufferWriteIndex = offset + bytes;
	readback->SetBuffer(m_textureReadbackBuffer, m_textureReadbackBufferPtr, offset);
	return readback;
}

uint32 s_vkCurrentUniqueId = 0;

uint64 VulkanRenderer::GenUniqueId()
{
	return ++s_vkCurrentUniqueId;
}

void VulkanRenderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
	auto& slot = m_streamoutState.buffer[bufferIndex];
	slot.enabled = true;
	slot.ringBufferOffset = ringBufferOffset;
}

void VulkanRenderer::streamout_begin()
{
}

void VulkanRenderer::streamout_rendererFinishDrawcall()
{
	for (auto& slot : m_streamoutState.buffer)
		slot.enabled = false;
}

void VulkanRenderer::buffer_bindVertexBuffers(std::span<BindBufferParam> bindings)
{
	cemu_assert_debug(!m_useHostMemoryForCache);
	for (const BindBufferParam& b : bindings)
	{
		auto& bound = m_state.currentVertexBinding[b.index];
		if (bound.offset == b.bindOffset)
			continue; // already bound at this offset
		bound.offset = b.bindOffset;
		const VkBuffer buffer = m_bufferCache;
		const VkDeviceSize offset = b.bindOffset;
		vkCmdBindVertexBuffers(m_state.currentCommandBuffer, b.index, 1, &buffer, &offset);
	}
}

void VulkanRenderer::buffer_bindVertexStrideWorkaroundBuffer(VkBuffer fixedBuffer, uint32 offset, uint32 bufferIndex, uint32 size)
{
	cemu_assert_debug(bufferIndex < Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS);
	// forget the cached binding so the next regular bind is not skipped
	m_state.currentVertexBinding[bufferIndex].offset = 0xFFFFFFFF;
	const VkDeviceSize bindOffset = offset;
	vkCmdBindVertexBuffers(m_state.currentCommandBuffer, bufferIndex, 1, &fixedBuffer, &bindOffset);
}

std::pair<VkBuffer, uint32> VulkanRenderer::buffer_genStrideWorkaroundVertexBuffer(MPTR buffer, uint32 size, uint32 oldStride)
{
	cemu_assert_debug(oldStride % 4 != 0);
	// copy each vertex into a slot padded up to the next multiple of 4 bytes
	const uint32 newStride = (oldStride / 4 + 1) * 4;
	const uint32 vertexCount = size / oldStride;
	const uint8* source = memory_getPointerFromPhysicalOffset(buffer);
	auto alloc = memoryManager->getMetalStrideWorkaroundAllocator().AllocateBufferMemory(vertexCount * newStride, 128);
	for (uint32 v = 0; v < vertexCount; v++)
		memcpy(alloc.memPtr + (size_t)v * newStride, source + (size_t)v * oldStride, oldStride);
	return { alloc.vkBuffer, alloc.bufferOffset };
}

void VulkanRenderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size)
{
	cemu_assert_debug(!m_useHostMemoryForCache);
	cemu_assert_debug(bufferIndex < 16);
	sint32 stage;
	switch (shaderType)
	{
	case LatteConst::ShaderType::Vertex: stage = VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX; break;
	case LatteConst::ShaderType::Geometry: stage = VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY; break;
	case LatteConst::ShaderType::Pixel: stage = VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT; break;
	default:
		cemu_assert_debug(false);
		return;
	}
	dynamicOffsetInfo.shaderUB[stage].uniformBufferOffset[bufferIndex] = offset;
}

void VulkanRenderer::bufferCache_init(const sint32 bufferSize)
{
	// importing guest memory as a host buffer is not enabled (m_useHostMemoryForCache is always false)
	m_importedMemBaseAddress = 0x10000000;
	if (!m_useHostMemoryForCache)
		memoryManager->CreateBuffer(bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, m_bufferCache, m_bufferCacheMemory);
}

void VulkanRenderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset)
{
	draw_endRenderPass();
	VKRSynchronizedRingAllocator& staging = memoryManager->getStagingAllocator();
	auto upload = staging.AllocateBufferMemory(size, 256);
	memcpy(upload.memPtr, buffer, size);
	staging.FlushReservation(upload);

	// staging data must be visible to the copy, and earlier reads of the target range must be done
	barrier_bufferRange<ANY_TRANSFER | HOST_WRITE, ANY_TRANSFER, BUFFER_SHADER_READ, TRANSFER_WRITE>(
		upload.vkBuffer, upload.bufferOffset, upload.size,
		m_bufferCache, bufferOffset, size);

	VkBufferCopy copy{};
	copy.srcOffset = upload.bufferOffset;
	copy.dstOffset = bufferOffset;
	copy.size = size;
	vkCmdCopyBuffer(m_state.currentCommandBuffer, upload.vkBuffer, m_bufferCache, 1, &copy);
	barrier_sequentializeTransfer();
}

void VulkanRenderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	cemu_assert_debug(!m_useHostMemoryForCache);
	draw_endRenderPass();
	barrier_sequentializeTransfer();
	cemu_assert_debug(!((srcOffset + size) > dstOffset && srcOffset < (dstOffset + size))); // ranges must not overlap
	VkBufferCopy copy{};
	copy.srcOffset = srcOffset;
	copy.dstOffset = dstOffset;
	copy.size = size;
	vkCmdCopyBuffer(m_state.currentCommandBuffer, m_bufferCache, m_bufferCache, 1, &copy);
	barrier_sequentializeTransfer();
}

void VulkanRenderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	draw_endRenderPass();
	VkBuffer target = m_bufferCache;
	if (m_useHostMemoryForCache)
	{
		// with imported host memory dstOffset is a physical address
		target = m_importedMem;
		dstOffset -= m_importedMemBaseAddress;
	}
	// transform feedback writes must land before the copy reads them; readers of the target must be done
	barrier_bufferRange<BUFFER_SHADER_WRITE, TRANSFER_READ, ANY_TRANSFER | BUFFER_SHADER_READ, TRANSFER_WRITE>(
		m_xfbRingBuffer, srcOffset, size,
		target, dstOffset, size);
	barrier_sequentializeTransfer();

	VkBufferCopy copy{};
	copy.srcOffset = srcOffset;
	copy.dstOffset = dstOffset;
	copy.size = size;
	vkCmdCopyBuffer(m_state.currentCommandBuffer, m_xfbRingBuffer, target, 1, &copy);
	barrier_sequentializeTransfer();
}

void VulkanRenderer::AppendOverlayDebugInfo()
{
	auto& vk = performanceMonitor.vk;
	ImGui::Text("--- Vulkan debug info ---");
	ImGui::Text("GfxPipelines   %u", vk.numGraphicPipelines.get());
	ImGui::Text("DescriptorSets %u", vk.numDescriptorSets.get());
	ImGui::Text("DS ImgSamplers %u", vk.numDescriptorSamplerTextures.get());
	ImGui::Text("DS DynUniform  %u", vk.numDescriptorDynUniformBuffers.get());
	ImGui::Text("DS StorageBuf  %u", vk.numDescriptorStorageBuffers.get());
	ImGui::Text("Images         %u", vk.numImages.get());
	ImGui::Text("ImageView      %u", vk.numImageViews.get());
	ImGui::Text("ImageSampler   %u", vk.numSamplers.get());
	ImGui::Text("RenderPass     %u", vk.numRenderPass.get());
	ImGui::Text("Framebuffer    %u", vk.numFramebuffer.get());
	m_spinlockDestructionQueue.lock();
	const unsigned int pendingDestruction = (unsigned int)m_destructionQueue.size();
	m_spinlockDestructionQueue.unlock();
	ImGui::Text("DestructionQ   %u", pendingDestruction);
	ImGui::Text("BeginRP/f      %u", vk.numBeginRenderpassPerFrame.get());
	ImGui::Text("Barriers/f     %u", vk.numDrawBarriersPerFrame.get());

	ImGui::Text("--- Cache debug info ---");
	uint32 heapSize = 0, allocatedSize = 0, allocationCount = 0;
	LatteBufferCache_getStats(heapSize, allocatedSize, allocationCount);
	ImGui::Text("Buffer");
	ImGui::SameLine(60.0f);
	ImGui::Text("%06uKB / %06uKB Allocs: %u", (allocatedSize + 1023) / 1024, (heapSize + 1023) / 1024, allocationCount);

	auto ringLine = [](const char* label, uint32 buffers, size_t total, size_t unused) {
		ImGui::Text("%s", label);
		ImGui::SameLine(60.0f);
		ImGui::Text("%06uKB / %06uKB Buffers: %u", ((uint32)(total - unused) + 1023) / 1024, ((uint32)total + 1023) / 1024, buffers);
	};
	uint32 buffers;
	size_t total, unused;
	memoryManager->getStagingAllocator().GetStats(buffers, total, unused);
	ringLine("Staging", buffers, total, unused);
	memoryManager->GetIndexAllocator().GetStats(buffers, total, unused);
	ringLine("Index", buffers, total, unused);

	ImGui::Text("--- Tex heaps ---");
	memoryManager->appendOverlayHeapDebugInfo();
}

// ---- VKR object wrappers ----

void VKRDestructibleObject::flagForCurrentCommandBuffer()
{
	m_lastCmdBufferId = VulkanRenderer::GetInstance()->GetCurrentCommandBufferId();
}

bool VKRDestructibleObject::canDestroy()
{
	// unreferenced and not used by any command buffer still in flight
	return m_refCount <= 0 && VulkanRenderer::GetInstance()->HasCommandBufferFinished(m_lastCmdBufferId);
}

VKRObjectTexture::VKRObjectTexture()
{
	performanceMonitor.vk.numImages.increment();
}

VKRObjectTexture::~VKRObjectTexture()
{
	VulkanRenderer* vkr = VulkanRenderer::GetInstance();
	if (m_allocation)
	{
		vkr->GetMemoryManager()->imageMemoryFree(m_allocation);
		m_allocation = nullptr;
	}
	if (m_image)
		vkDestroyImage(vkr->GetLogicalDevice(), m_image, nullptr);
	performanceMonitor.vk.numImages.decrement();
}

VKRObjectTextureView::VKRObjectTextureView(VKRObjectTexture* tex, VkImageView view)
{
	m_textureImageView = view;
	addRef(tex);
	performanceMonitor.vk.numImageViews.increment();
}

VKRObjectTextureView::~VKRObjectTextureView()
{
	const VkDevice device = VulkanRenderer::GetInstance()->GetLogicalDevice();
	for (VkSampler sampler : m_textureDefaultSampler)
	{
		if (sampler != VK_NULL_HANDLE)
			vkDestroySampler(device, sampler, nullptr);
	}
	vkDestroyImageView(device, m_textureImageView, nullptr);
	performanceMonitor.vk.numImageViews.decrement();
}

namespace
{
	// key for the sampler cache: every field that changes sampling, ignoring fields
	// that are disabled (max anisotropy without anisotropy, compare op without compare)
	class SamplerKeyBuilder
	{
	public:
		template<typename T>
		void Add(const T& value)
		{
			static_assert(sizeof(T) <= 8);
			uint64 bits = 0;
			memcpy(&bits, &value, sizeof(T));
			m_key = (m_key ^ bits) * 0x00000100000001B3ull + 0x9E3779B97F4A7C15ull;
			m_key ^= m_key >> 29;
		}
		uint64 Get() const { return m_key; }
	private:
		uint64 m_key = 0x6A09E667F3BCC908ull;
	};

	uint64 SamplerCacheKey(const VkSamplerCreateInfo& info)
	{
		cemu_assert_debug(info.sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
		SamplerKeyBuilder k;
		k.Add(info.flags);
		k.Add(info.magFilter);
		k.Add(info.minFilter);
		k.Add(info.mipmapMode);
		k.Add(info.addressModeU);
		k.Add(info.addressModeV);
		k.Add(info.addressModeW);
		k.Add(info.mipLodBias);
		k.Add(info.anisotropyEnable);
		if (info.anisotropyEnable == VK_TRUE)
			k.Add(info.maxAnisotropy);
		k.Add(info.compareEnable);
		if (info.compareEnable == VK_TRUE)
			k.Add(info.compareOp);
		k.Add(info.minLod);
		k.Add(info.maxLod);
		k.Add(info.borderColor);
		k.Add(info.unnormalizedCoordinates);
		for (auto* ext = (const VkBaseInStructure*)info.pNext; ext; ext = ext->pNext)
		{
			if (ext->sType != VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT)
			{
				cemu_assert_unimplemented();
				continue;
			}
			const auto* border = (const VkSamplerCustomBorderColorCreateInfoEXT*)ext;
			for (uint32 c = 0; c < 4; c++)
				k.Add(border->customBorderColor.uint32[c]);
		}
		return k.Get();
	}
}

std::unordered_map<uint64, VKRObjectSampler*> VKRObjectSampler::s_samplerCache;

VKRObjectSampler::VKRObjectSampler(VkSamplerCreateInfo* samplerInfo)
{
	VulkanRenderer* vkr = VulkanRenderer::GetInstance();
	if (vkCreateSampler(vkr->GetLogicalDevice(), samplerInfo, nullptr, &m_sampler) != VK_SUCCESS)
		vkr->UnrecoverableError("Failed to create texture sampler");
	performanceMonitor.vk.numSamplers.increment();
	m_hash = SamplerCacheKey(*samplerInfo);
}

VKRObjectSampler::~VKRObjectSampler()
{
	vkDestroySampler(VulkanRenderer::GetInstance()->GetLogicalDevice(), m_sampler, nullptr);
	performanceMonitor.vk.numSamplers.decrement();
	s_samplerCache.erase(m_hash);
}

void VKRObjectSampler::RefCountReachedZero()
{
	VulkanRenderer::GetInstance()->ReleaseDestructibleObject(this);
}

VKRObjectSampler* VKRObjectSampler::GetOrCreateSampler(VkSamplerCreateInfo* samplerInfo)
{
	const uint64 key = SamplerCacheKey(*samplerInfo);
	if (const auto it = s_samplerCache.find(key); it != s_samplerCache.end())
		return it->second;
	VKRObjectSampler* sampler = new VKRObjectSampler(samplerInfo);
	s_samplerCache.emplace(key, sampler);
	return sampler;
}

void VKRObjectSampler::DestroyCache()
{
	// normally empty by now since every sampler owner is already gone
	cemu_assert_debug(s_samplerCache.empty());
	std::vector<VKRObjectSampler*> leftovers;
	for (auto& entry : s_samplerCache)
	{
		cemu_assert_debug(entry.second->m_refCount == 0);
		leftovers.push_back(entry.second);
	}
	for (VKRObjectSampler* sampler : leftovers)
		delete sampler; // the destructor unregisters itself
	s_samplerCache.clear();
}

VKRObjectRenderPass::VKRObjectRenderPass(AttachmentInfo_t& attachmentInfo, sint32 colorAttachmentCount)
{
	VulkanRenderer* vkr = VulkanRenderer::GetInstance();
	const bool feedbackLoop = vkr->UseAttachmentFeedbackLoop();
	// attachments stay in one layout for their whole life: the feedback loop layout when available
	const VkImageLayout layout = feedbackLoop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;

	auto inUse = [](const auto& slot) { return slot.isPresent || slot.viewObj != nullptr; };

	// summary of the formats, folded into pipeline state hashes
	uint64 formatKey = 0xCBF29CE484222325ull;
	auto mixKey = [&formatKey](uint64 v) { formatKey = (formatKey ^ v) * 0x00000100000001B3ull; };
	for (int i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
	{
		if (!inUse(attachmentInfo.colorAttachment[i]))
			continue;
		mixKey((uint64)i + 1);
		mixKey((uint64)attachmentInfo.colorAttachment[i].format);
	}
	if (inUse(attachmentInfo.depthAttachment))
	{
		mixKey(0xD0);
		mixKey((uint64)attachmentInfo.depthAttachment.format);
	}
	m_hashForPipeline = formatKey;

	std::vector<VkAttachmentDescription> descriptions;
	std::array<VkAttachmentReference, Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS> colorRefs{};
	cemu_assert(colorAttachmentCount <= (sint32)colorRefs.size());
	auto describe = [&descriptions, layout](VkFormat format, bool keepStencil) {
		VkAttachmentDescription d{};
		d.format = format;
		d.samples = VK_SAMPLE_COUNT_1_BIT;
		d.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		d.stencilLoadOp = keepStencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		d.stencilStoreOp = keepStencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		d.initialLayout = layout;
		d.finalLayout = layout;
		descriptions.push_back(d);
		return (uint32)descriptions.size() - 1;
	};

	for (int i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
	{
		const auto& slot = attachmentInfo.colorAttachment[i];
		if (!inUse(slot))
		{
			colorRefs[i].attachment = VK_ATTACHMENT_UNUSED;
			m_colorAttachmentFormat[i] = VK_FORMAT_UNDEFINED;
			continue;
		}
		m_colorAttachmentFormat[i] = slot.format;
		colorRefs[i].attachment = describe(slot.format, false);
		colorRefs[i].layout = layout;
	}

	VkAttachmentReference depthRef{};
	if (inUse(attachmentInfo.depthAttachment))
	{
		m_depthAttachmentFormat = attachmentInfo.depthAttachment.format;
		depthRef.attachment = describe(attachmentInfo.depthAttachment.format, attachmentInfo.depthAttachment.hasStencil);
		depthRef.layout = layout;
	}
	else
	{
		m_depthAttachmentFormat = VK_FORMAT_UNDEFINED;
		depthRef.attachment = VK_ATTACHMENT_UNUSED;
	}

	// the colour reference list always spans colorAttachmentCount slots, unused ones marked as such
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = colorAttachmentCount;
	subpass.pColorAttachments = colorRefs.data();
	subpass.pDepthStencilAttachment = &depthRef;

	VkRenderPassCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = (uint32)descriptions.size();
	info.pAttachments = descriptions.data();
	info.subpassCount = 1;
	info.pSubpasses = &subpass;

	// With the feedback loop extension, a self dependency makes attachment writes visible to
	// later fragment shader reads in the same pass. Without it no dependency is declared and
	// the renderer places explicit barriers between draws instead.
	VkSubpassDependency selfDependency{};
	if (feedbackLoop)
	{
		constexpr VkPipelineStageFlags fragmentTests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		selfDependency.srcSubpass = 0;
		selfDependency.dstSubpass = 0;
		selfDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragmentTests;
		selfDependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragmentTests;
		selfDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		selfDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		selfDependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT;
		info.dependencyCount = 1;
		info.pDependencies = &selfDependency;
	}

	if (vkCreateRenderPass(vkr->GetLogicalDevice(), &info, nullptr, &m_renderPass) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan-Error: Failed to create render pass");
		throw std::runtime_error("failed to create render pass!");
	}

	// keep the attachment views alive as long as this pass
	for (int i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
	{
		if (attachmentInfo.colorAttachment[i].viewObj)
			addRef(attachmentInfo.colorAttachment[i].viewObj);
	}
	if (attachmentInfo.depthAttachment.viewObj)
		addRef(attachmentInfo.depthAttachment.viewObj);
	performanceMonitor.vk.numRenderPass.increment();
}

VKRObjectRenderPass::~VKRObjectRenderPass()
{
	if (m_renderPass != VK_NULL_HANDLE)
		vkDestroyRenderPass(VulkanRenderer::GetInstance()->GetLogicalDevice(), m_renderPass, nullptr);
	performanceMonitor.vk.numRenderPass.decrement();
}

VKRObjectFramebuffer::VKRObjectFramebuffer(VKRObjectRenderPass* renderPass, std::span<VKRObjectTextureView*> attachments, Vector2i size)
{
	std::array<VkImageView, 16> views;
	cemu_assert(attachments.size() < views.size());
	for (size_t i = 0; i < attachments.size(); i++)
		views[i] = attachments[i]->m_textureImageView;

	VkFramebufferCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	info.renderPass = renderPass->m_renderPass;
	info.attachmentCount = (uint32)attachments.size();
	info.pAttachments = views.data();
	info.width = size.x;
	info.height = size.y;
	info.layers = 1;
	if (vkCreateFramebuffer(VulkanRenderer::GetInstance()->GetLogicalDevice(), &info, nullptr, &m_frameBuffer) != VK_SUCCESS)
		throw std::runtime_error("failed to create framebuffer!");

	addRef(renderPass);
	for (VKRObjectTextureView* view : attachments)
		addRef(view);
	performanceMonitor.vk.numFramebuffer.increment();
}

VKRObjectFramebuffer::~VKRObjectFramebuffer()
{
	if (m_frameBuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(VulkanRenderer::GetInstance()->GetLogicalDevice(), m_frameBuffer, nullptr);
	performanceMonitor.vk.numFramebuffer.decrement();
}

VKRObjectPipeline::VKRObjectPipeline()
{
}

void VKRObjectPipeline::SetPipeline(VkPipeline newPipeline)
{
	if (newPipeline == m_pipeline)
		return;
	cemu_assert_debug(m_pipeline == VK_NULL_HANDLE); // a pipeline is assigned once
	const bool hadPipeline = m_pipeline != VK_NULL_HANDLE;
	const bool hasPipeline = newPipeline != VK_NULL_HANDLE;
	if (hasPipeline && !hadPipeline)
		performanceMonitor.vk.numGraphicPipelines.increment();
	else if (hadPipeline && !hasPipeline)
		performanceMonitor.vk.numGraphicPipelines.decrement();
	m_pipeline = newPipeline;
}

VKRObjectPipeline::~VKRObjectPipeline()
{
	const VkDevice device = VulkanRenderer::GetInstance()->GetLogicalDevice();
	if (m_pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, m_pipeline, nullptr);
		performanceMonitor.vk.numGraphicPipelines.decrement();
	}
	for (VkDescriptorSetLayout layout : { m_vertexDSL, m_pixelDSL, m_geometryDSL })
	{
		if (layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(device, layout, nullptr);
	}
	if (m_pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
}

VKRObjectDescriptorSet::VKRObjectDescriptorSet()
{
	performanceMonitor.vk.numDescriptorSets.increment();
}

VKRObjectDescriptorSet::~VKRObjectDescriptorSet()
{
	VulkanRenderer* vkr = VulkanRenderer::GetInstance();
	vkFreeDescriptorSets(vkr->GetLogicalDevice(), vkr->GetDescriptorPool(), 1, &descriptorSet);
	performanceMonitor.vk.numDescriptorSets.decrement();
}
