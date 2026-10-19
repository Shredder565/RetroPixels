// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "WindowSystem.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"

#include <imgui.h>
#include "imgui/imgui_extension.h"

std::unique_ptr<Renderer> g_renderer;

bool Renderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const
{
	usageInMB = -1;
	totalInMB = -1;
#if BOOST_OS_WINDOWS
	DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
	if (m_dxgi_wrapper && m_dxgi_wrapper->QueryVideoMemoryInfo(memInfo))
	{
		// decimal megabytes, like the driver tools report them
		totalInMB = (int)(memInfo.Budget / 1000000ull);
		usageInMB = (int)(memInfo.CurrentUsage / 1000000ull);
		return true;
	}
#endif
	return false;
}

static ImGuiContext* CreateOverlayContext(ImFontAtlas* sharedFonts)
{
	ImGuiContext* ctx = ImGui::CreateContext(sharedFonts);
	ImGui::SetCurrentContext(ctx);
	ImGuiIO& io = ImGui::GetIO();
	// the overlay has no persistent window layout
	io.IniFilename = nullptr;
	io.WantSaveIniSettings = false;
	return ctx;
}

void Renderer::Initialize()
{
	// TV and GamePad overlays have separate imgui contexts that share one font atlas
	imguiFontAtlas = new ImFontAtlas();
	imguiFontAtlas->AddFontDefault();
	imguiTVContext = CreateOverlayContext(imguiFontAtlas);
	imguiPadContext = CreateOverlayContext(imguiFontAtlas);
}

void Renderer::Shutdown()
{
	ImGui::DestroyContext(imguiTVContext);
	ImGui::DestroyContext(imguiPadContext);
	ImGui_ClearFonts();
	delete imguiFontAtlas;
}

bool Renderer::ImguiBegin(bool mainWindow)
{
	sint32 width = 0;
	sint32 height = 0;
	if (mainWindow)
		WindowSystem::GetWindowPhysSize(width, height);
	else if (!WindowSystem::IsPadWindowOpen())
		return false;
	else
		WindowSystem::GetPadWindowPhysSize(width, height);
	if (width == 0 || height == 0)
		return false;

	ImGui::SetCurrentContext(mainWindow ? imguiTVContext : imguiPadContext);
	ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
	ImGui_PrecacheFonts();
	return true;
}

uint8 Renderer::SRGBComponentToRGB(uint8 ci)
{
	// sRGB EOTF (IEC 61966-2-1)
	const float encoded = (float)ci / 255.0f;
	float linear = encoded <= 0.04045 ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
	if (linear > 1.0f)
		linear = 1.0f;
	return (uint8)(linear * 255.0f);
}

uint8 Renderer::RGBComponentToSRGB(uint8 cli)
{
	// inverse transfer function; the exponent is the 5/12 approximation used for screenshots
	const float linear = (float)cli / 255.0f;
	float encoded = linear < 0.0031308 ? linear * 12.92f : 1.055f * std::pow(linear, 0.41666f) - 0.055f;
	encoded = std::clamp(encoded, 0.0f, 1.0f);
	return (uint8)(encoded * 255.0f);
}

void Renderer::RequestScreenshot(ScreenshotSaveFunction onSaveScreenshot)
{
	m_on_save_screenshot = std::move(onSaveScreenshot);
	m_screenshot_requested = true;
}

void Renderer::CancelScreenshotRequest()
{
	m_on_save_screenshot = {};
	m_screenshot_requested = false;
}

void Renderer::SaveScreenshot(const std::vector<uint8>& rgb_data, int width, int height, bool mainWindow)
{
	// take ownership of the pending request now, the save itself (encoding, file IO, clipboard) runs off the render thread
	const bool wasRequested = m_screenshot_requested;
	m_screenshot_requested = false;
	ScreenshotSaveFunction saveFunc = std::move(m_on_save_screenshot);
	m_on_save_screenshot = {};
	std::thread worker([pixels = rgb_data, width, height, mainWindow, wasRequested, saveFunc = std::move(saveFunc)]() {
		if (!wasRequested || !saveFunc)
			return;
		std::optional<std::string> message = saveFunc(pixels, width, height, mainWindow);
		if (message)
			LatteOverlay_pushNotification(*message, 2500);
	});
	worker.detach();
}
