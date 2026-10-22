// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "util/math/vector2.h"
#include <vulkan/vulkan_core.h>

// Presentation state of one output window (TV or GamePad): its surface, the swapchain created on it and the
// per-image objects (views, framebuffers, semaphores) the renderer needs to draw into and present the images.
struct SwapchainInfoVk
{
	// numeric values are stored in the settings file (GeneralSettings2 vsync choice)
	enum class VSync
	{
		Immediate = 0,
		FIFO = 1,
		MAILBOX = 2,
		SYNC_AND_LIMIT = 3, // emulated vsync follows the monitor's vblank, events faster than the console rate are dropped
	};

	struct SwapchainSupportDetails
	{
		VkSurfaceCapabilitiesKHR capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};

	SwapchainInfoVk(bool mainWindow, Vector2i size);
	SwapchainInfoVk(const SwapchainInfoVk&) = delete;
	SwapchainInfoVk(SwapchainInfoVk&&) noexcept = default;
	~SwapchainInfoVk();

	// builds the swapchain and everything attached to it, Cleanup releases it again (the surface stays)
	void Create();
	void Cleanup();
	bool IsValid() const;

	// fence of the last successful acquire
	void WaitAvailableFence();
	void ResetAvailableFence() const;

	// acquires the next image into swapchainImageIndex, false on timeout or when the swapchain is out of date
	bool AcquireImage();
	// the semaphore signalled by the last acquire. It may be waited on only once, so it is handed out a single time
	// and later calls return VK_NULL_HANDLE
	VkSemaphore ConsumeAcquireSemaphore();

	VkExtent2D getExtent() const { return m_actualExtent; }

	static void UnrecoverableError(const char* errMsg);
	static SwapchainSupportDetails QuerySwapchainSupport(VkSurfaceKHR surface, const VkPhysicalDevice& device);

	VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes);
	VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
	VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
	VkSwapchainCreateInfoKHR CreateSwapchainCreateInfo(VkSurfaceKHR surface, const SwapchainSupportDetails& swapchainSupport, const VkSurfaceFormatKHR& surfaceFormat, uint32 imageCount, const VkExtent2D& extent);

	bool mainWindow{};

	bool m_shouldRecreate = false;
	VSync m_vsyncState = VSync::Immediate;
	bool hasDefinedSwapchainImage{}; // false until something was drawn into the acquired image

	VkInstance m_instance{};
	VkPhysicalDevice m_physicalDevice{};
	VkDevice m_logicalDevice{};
	VkSurfaceKHR m_surface{};
	VkSurfaceFormatKHR m_surfaceFormat{};
	VkSwapchainKHR m_swapchain{};
	Vector2i m_desiredExtent{};
	VkExtent2D m_actualExtent{};
	uint32 swapchainImageIndex = (uint32)-1;
	uint64 m_presentId = 1;
	uint64 m_queueDepth = 0; // frames handed to the presentation engine that were not shown yet
	uint64 m_maxQueued = 0; // limit for m_queueDepth, 0 means unlimited

	// one entry per swapchain image, indexed by swapchainImageIndex
	std::vector<VkImage> m_swapchainImages;
	std::vector<VkImageView> m_swapchainImageViews;
	std::vector<VkFramebuffer> m_swapchainFramebuffers;
	std::vector<VkSemaphore> m_presentSemaphores;

	VkRenderPass m_swapchainRenderPass = nullptr;

private:
	void CreateRenderPass();
	void CreateImageObjects();
	void CreateSyncObjects();

	// acquire semaphores rotate independently of the image index
	std::vector<VkSemaphore> m_acquireSemaphores;
	uint32 m_acquireIndex = 0;
	VkSemaphore m_currentSemaphore = VK_NULL_HANDLE;
	VkFence m_imageAvailableFence{};
	VkFence m_awaitableFence = VK_NULL_HANDLE; // set while an acquire is pending on m_imageAvailableFence

	std::array<uint32, 2> m_swapchainQueueFamilyIndices;
};
