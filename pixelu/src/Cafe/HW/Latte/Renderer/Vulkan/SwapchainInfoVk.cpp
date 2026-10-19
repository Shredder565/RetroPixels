// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "SwapchainInfoVk.h"

#include "config/CemuConfig.h"
#include "WindowSystem.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteTiming.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"

namespace
{
	constexpr uint64 kAcquireTimeoutNs = 1'000'000'000;

	// fills a vector through the usual Vulkan count-then-data query, throwing with the given text on failure
	template<typename T, typename F>
	std::vector<T> QueryList(F&& query, const char* apiName, const char* countError, const char* dataError)
	{
		std::vector<T> items;
		uint32_t count = 0;
		VkResult res = query(&count, (T*)nullptr);
		if (res != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "{} failed. Error {}", apiName, (sint32)res);
			throw std::runtime_error(fmt::format("{}: {}", countError, res));
		}
		if (count == 0)
			return items;
		items.resize(count);
		res = query(&count, items.data());
		if (res != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "{} failed. Error {}", apiName, (sint32)res);
			throw std::runtime_error(fmt::format("{}: {}", dataError, res));
		}
		return items;
	}
}

SwapchainInfoVk::SwapchainInfoVk(bool mainWindow, Vector2i size) : mainWindow(mainWindow), m_desiredExtent(size)
{
	auto& windows = WindowSystem::GetWindowInfo();
	VulkanRenderer* renderer = VulkanRenderer::GetInstance();
	m_instance = renderer->GetVkInstance();
	m_logicalDevice = renderer->GetLogicalDevice();
	m_physicalDevice = renderer->GetPhysicalDevice();
	m_surface = renderer->CreateFramebufferSurface(m_instance, mainWindow ? windows.canvas_main : windows.canvas_pad);
}

SwapchainInfoVk::~SwapchainInfoVk()
{
	Cleanup();
	if (m_surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
}

void SwapchainInfoVk::Create()
{
	const SwapchainSupportDetails support = QuerySwapchainSupport(m_surface, m_physicalDevice);
	m_surfaceFormat = ChooseSurfaceFormat(support.formats);
	m_actualExtent = ChooseSwapExtent(support.capabilities);

	// some drivers misbehave with a single image, so ask for two unless the surface caps the count lower
	uint32_t imageCount = std::max<uint32_t>(support.capabilities.minImageCount, 2);
	if (support.capabilities.maxImageCount != 0 && imageCount > support.capabilities.maxImageCount)
		imageCount = support.capabilities.maxImageCount;
	if (imageCount < 2)
		cemuLog_log(LogType::Force, "Vulkan: Swapchain image count less than 2 may cause problems");

	VkSwapchainCreateInfoKHR createInfo = CreateSwapchainCreateInfo(m_surface, support, m_surfaceFormat, imageCount, m_actualExtent);
	createInfo.oldSwapchain = VK_NULL_HANDLE;
	// images are also targets of transfer clears
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (vkCreateSwapchainKHR(m_logicalDevice, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS)
		UnrecoverableError("Error attempting to create a swapchain");

	if (vkGetSwapchainImagesKHR(m_logicalDevice, m_swapchain, &imageCount, nullptr) != VK_SUCCESS)
		UnrecoverableError("Error attempting to retrieve the count of swapchain images");
	m_swapchainImages.resize(imageCount);
	if (vkGetSwapchainImagesKHR(m_logicalDevice, m_swapchain, &imageCount, m_swapchainImages.data()) != VK_SUCCESS)
		UnrecoverableError("Error attempting to retrieve swapchain images");

	CreateRenderPass();
	CreateImageObjects();
	CreateSyncObjects();

	m_acquireIndex = 0;
	hasDefinedSwapchainImage = false;
	m_queueDepth = 0;
}

void SwapchainInfoVk::CreateRenderPass()
{
	// single colour attachment, previous contents are not needed and the image ends up ready for presentation
	VkAttachmentDescription attachment{};
	attachment.format = m_surfaceFormat.format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	const VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;

	VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.attachmentCount = 1;
	info.pAttachments = &attachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	if (vkCreateRenderPass(m_logicalDevice, &info, nullptr, &m_swapchainRenderPass) != VK_SUCCESS)
		UnrecoverableError("Failed to create renderpass for swapchain");
}

void SwapchainInfoVk::CreateImageObjects()
{
	const size_t count = m_swapchainImages.size();
	m_swapchainImageViews.resize(count);
	for (size_t i = 0; i < count; i++)
	{
		VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		info.image = m_swapchainImages[i];
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = m_surfaceFormat.format;
		info.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
		info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		if (vkCreateImageView(m_logicalDevice, &info, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS)
			UnrecoverableError("Failed to create imageviews for swapchain");
	}

	m_swapchainFramebuffers.resize(count);
	for (size_t i = 0; i < count; i++)
	{
		VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		info.renderPass = m_swapchainRenderPass;
		info.attachmentCount = 1;
		info.pAttachments = &m_swapchainImageViews[i];
		info.width = m_actualExtent.width;
		info.height = m_actualExtent.height;
		info.layers = 1;
		if (vkCreateFramebuffer(m_logicalDevice, &info, nullptr, &m_swapchainFramebuffers[i]) != VK_SUCCESS)
			UnrecoverableError("Failed to create framebuffer for swapchain");
	}
}

void SwapchainInfoVk::CreateSyncObjects()
{
	const VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	m_presentSemaphores.resize(m_swapchainImages.size());
	for (VkSemaphore& s : m_presentSemaphores)
	{
		if (vkCreateSemaphore(m_logicalDevice, &semaphoreInfo, nullptr, &s) != VK_SUCCESS)
			UnrecoverableError("Failed to create semaphore for swapchain present");
	}
	m_acquireSemaphores.resize(m_swapchainImages.size());
	for (VkSemaphore& s : m_acquireSemaphores)
	{
		if (vkCreateSemaphore(m_logicalDevice, &semaphoreInfo, nullptr, &s) != VK_SUCCESS)
			UnrecoverableError("Failed to create semaphore for swapchain acquire");
	}

	VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if (vkCreateFence(m_logicalDevice, &fenceInfo, nullptr, &m_imageAvailableFence) != VK_SUCCESS)
		UnrecoverableError("Failed to create fence for swapchain");
}

void SwapchainInfoVk::Cleanup()
{
	m_swapchainImages.clear();

	for (VkSemaphore s : m_acquireSemaphores)
		vkDestroySemaphore(m_logicalDevice, s, nullptr);
	m_acquireSemaphores.clear();
	for (VkSemaphore s : m_presentSemaphores)
		vkDestroySemaphore(m_logicalDevice, s, nullptr);
	m_presentSemaphores.clear();

	if (m_swapchainRenderPass)
	{
		vkDestroyRenderPass(m_logicalDevice, m_swapchainRenderPass, nullptr);
		m_swapchainRenderPass = nullptr;
	}
	for (VkImageView v : m_swapchainImageViews)
		vkDestroyImageView(m_logicalDevice, v, nullptr);
	m_swapchainImageViews.clear();
	for (VkFramebuffer fb : m_swapchainFramebuffers)
		vkDestroyFramebuffer(m_logicalDevice, fb, nullptr);
	m_swapchainFramebuffers.clear();

	if (m_imageAvailableFence)
	{
		// an acquire may still be in flight
		WaitAvailableFence();
		vkDestroyFence(m_logicalDevice, m_imageAvailableFence, nullptr);
		m_imageAvailableFence = nullptr;
	}
	if (m_swapchain)
	{
		vkDestroySwapchainKHR(m_logicalDevice, m_swapchain, nullptr);
		m_swapchain = VK_NULL_HANDLE;
	}
}

bool SwapchainInfoVk::IsValid() const
{
	return m_swapchain != VK_NULL_HANDLE && !m_acquireSemaphores.empty();
}

void SwapchainInfoVk::WaitAvailableFence()
{
	if (m_awaitableFence != VK_NULL_HANDLE)
		vkWaitForFences(m_logicalDevice, 1, &m_awaitableFence, VK_TRUE, UINT64_MAX);
	m_awaitableFence = VK_NULL_HANDLE;
}

void SwapchainInfoVk::ResetAvailableFence() const
{
	vkResetFences(m_logicalDevice, 1, &m_imageAvailableFence);
}

VkSemaphore SwapchainInfoVk::ConsumeAcquireSemaphore()
{
	return std::exchange(m_currentSemaphore, VK_NULL_HANDLE);
}

bool SwapchainInfoVk::AcquireImage()
{
	ResetAvailableFence();
	VkSemaphore semaphore = m_acquireSemaphores[m_acquireIndex];
	const VkResult res = vkAcquireNextImageKHR(m_logicalDevice, m_swapchain, kAcquireTimeoutNs, semaphore, m_imageAvailableFence, &swapchainImageIndex);
	// a suboptimal swapchain still delivered an image, but it should be rebuilt soon
	if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
		m_shouldRecreate = true;
	if (res == VK_TIMEOUT || res < 0)
	{
		swapchainImageIndex = (uint32)-1;
		if (res < 0 && res != VK_ERROR_OUT_OF_DATE_KHR)
			throw std::runtime_error(fmt::format("Failed to acquire next image: {}", res));
		return false;
	}
	m_currentSemaphore = semaphore;
	m_awaitableFence = m_imageAvailableFence;
	m_acquireIndex = (uint32)((m_acquireIndex + 1) % m_swapchainImages.size());
	return true;
}

void SwapchainInfoVk::UnrecoverableError(const char* errMsg)
{
	cemuLog_log(LogType::Force, "Unrecoverable error in Vulkan swapchain");
	cemuLog_log(LogType::Force, "Msg: {}", errMsg);
	throw std::runtime_error(errMsg);
}

SwapchainInfoVk::SwapchainSupportDetails SwapchainInfoVk::QuerySwapchainSupport(VkSurfaceKHR surface, const VkPhysicalDevice& device)
{
	SwapchainSupportDetails details{};
	const VkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
	if (res != VK_SUCCESS)
	{
		// a lost surface is expected while a window closes, so it is not logged
		if (res != VK_ERROR_SURFACE_LOST_KHR)
			cemuLog_log(LogType::Force, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed. Error {}", (sint32)res);
		throw std::runtime_error(fmt::format("Unable to retrieve physical device surface capabilities: {}", res));
	}
	details.formats = QueryList<VkSurfaceFormatKHR>(
		[&](uint32_t* count, VkSurfaceFormatKHR* out) { return vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, count, out); },
		"vkGetPhysicalDeviceSurfaceFormatsKHR",
		"Unable to retrieve the number of formats for a surface on a physical device",
		"Unable to retrieve the formats for a surface on a physical device");
	details.presentModes = QueryList<VkPresentModeKHR>(
		[&](uint32_t* count, VkPresentModeKHR* out) { return vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, count, out); },
		"vkGetPhysicalDeviceSurfacePresentModesKHR",
		"Unable to retrieve the count of present modes for a surface on a physical device",
		"Unable to retrieve the present modes for a surface on a physical device");
	return details;
}

VkSurfaceFormatKHR SwapchainInfoVk::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
	constexpr VkSurfaceFormatKHR preferred{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
	// a single UNDEFINED entry means the surface accepts any format
	if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED)
		return preferred;
	auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
		return f.format == preferred.format && f.colorSpace == preferred.colorSpace;
	});
	return it != formats.end() ? *it : formats.front();
}

VkExtent2D SwapchainInfoVk::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const
{
	// the special value 0xFFFFFFFF lets the swapchain decide the size
	if (capabilities.currentExtent.width != 0xFFFFFFFFu)
		return capabilities.currentExtent;
	auto fit = [](uint32 want, uint32 lo, uint32 hi) { return std::max(lo, std::min(hi, want)); };
	return VkExtent2D{
		fit((uint32)m_desiredExtent.x, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
		fit((uint32)m_desiredExtent.y, capabilities.minImageExtent.height, capabilities.maxImageExtent.height) };
}

VkPresentModeKHR SwapchainInfoVk::ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes)
{
	auto supported = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
	m_maxQueued = 0;
	switch ((VSync)GetConfig().vsync.GetValue())
	{
	case VSync::MAILBOX:
		if (supported(VK_PRESENT_MODE_MAILBOX_KHR))
			return VK_PRESENT_MODE_MAILBOX_KHR;
		cemuLog_log(LogType::Force, "Vulkan: Can't find mailbox present mode");
		break;
	case VSync::Immediate:
		if (supported(VK_PRESENT_MODE_IMMEDIATE_KHR))
			return VK_PRESENT_MODE_IMMEDIATE_KHR;
		cemuLog_log(LogType::Force, "Vulkan: Can't find immediate present mode");
		break;
	case VSync::SYNC_AND_LIMIT:
		// pacing comes from the host vblank thread, presentation itself stays FIFO without a queue limit
		LatteTiming_EnableHostDrivenVSync();
		return VK_PRESENT_MODE_FIFO_KHR;
	default:
		break;
	}
	// plain FIFO, also the fallback when the requested mode is missing: keep at most one frame queued
	m_maxQueued = 1;
	return VK_PRESENT_MODE_FIFO_KHR;
}

VkSwapchainCreateInfoKHR SwapchainInfoVk::CreateSwapchainCreateInfo(VkSurfaceKHR surface, const SwapchainSupportDetails& swapchainSupport, const VkSurfaceFormatKHR& surfaceFormat, uint32 imageCount, const VkExtent2D& extent)
{
	VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = surface;
	info.minImageCount = imageCount;
	info.imageFormat = surfaceFormat.format;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	const VulkanRenderer::QueueFamilyIndices families = VulkanRenderer::GetInstance()->FindQueueFamilies(surface, m_physicalDevice);
	m_swapchainQueueFamilyIndices = { (uint32)families.graphicsFamily, (uint32)families.presentFamily };
	if (families.graphicsFamily == families.presentFamily)
	{
		info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}
	else
	{
		// rendering and presentation happen on different queue families
		info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		info.queueFamilyIndexCount = (uint32)m_swapchainQueueFamilyIndices.size();
		info.pQueueFamilyIndices = m_swapchainQueueFamilyIndices.data();
	}
	info.preTransform = swapchainSupport.capabilities.currentTransform;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = ChoosePresentMode(swapchainSupport.presentModes);
	info.clipped = VK_TRUE;
	cemuLog_logDebug(LogType::Force, "vulkan presentation mode: {}", info.presentMode);
	return info;
}
