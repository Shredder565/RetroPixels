// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "imgui_extension.h"
#include "WindowSystem.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "resource/IconsFontAwesome5.h"
#include "resource/resource.h"
#include "input/InputManager.h"

// ---- rotation of draw list vertices ----

namespace
{
	// ==== imrotate begin ====
	// The "center" is taken from the leftmost and the rightmost vertex
	// (x and y of each), not from a full bounding box. On equal x the last
	// leftmost and the first rightmost vertex count. No vertices gives (0,0).
	ImVec2 CenterOfVertices(const ImDrawVert* verts, int count)
	{
		ImVec2 left(FLT_MAX, FLT_MAX), right(-FLT_MAX, -FLT_MAX);
		for (int i = 0; i < count; i++)
		{
			const ImVec2& p = verts[i].pos;
			if (p.x <= left.x)
				left = p;
			if (p.x > right.x)
				right = p;
		}
		return ImVec2((left.x + right.x) / 2, (left.y + right.y) / 2);
	}

	// Turns the vertices around center by (pi/2 - rad), the convention the
	// overlay's callers are written for.
	void TurnVertices(ImDrawVert* verts, int count, float rad, ImVec2 center)
	{
		const float c = std::sin(rad); // cos(pi/2 - rad)
		const float s = std::cos(rad); // sin(pi/2 - rad)
		for (int i = 0; i < count; i++)
		{
			const float dx = verts[i].pos.x - center.x;
			const float dy = verts[i].pos.y - center.y;
			verts[i].pos = ImVec2(center.x + dx * c - dy * s, center.y + dx * s + dy * c);
		}
	}
	// ==== imrotate end ====

	int s_rotationFirstVertex = 0;

	ImDrawVert* RotatedVertices(int& count)
	{
		auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
		count = vertices.Size - s_rotationFirstVertex;
		return vertices.Data + s_rotationFirstVertex;
	}
}

void ImRotateStart()
{
	s_rotationFirstVertex = ImGui::GetWindowDrawList()->VtxBuffer.Size;
}

ImVec2 ImRotationCenter()
{
	int count;
	const ImDrawVert* verts = RotatedVertices(count);
	return CenterOfVertices(verts, count);
}

void ImRotateEnd(float rad, ImVec2 center)
{
	int count;
	ImDrawVert* verts = RotatedVertices(count);
	TurnVertices(verts, count, rad, center);
}

// ---- fonts ----

uint8* extractCafeDefaultFont(sint32* size);
#if !BOOST_OS_WINDOWS
extern int const g_fontawesome_size;
extern char const g_fontawesome_data[];
#endif

namespace
{
	// the console's system font, loaded once and shared by every size
	uint8* s_systemFont = nullptr;
	sint32 s_systemFontSize = 0;

	std::unordered_map<int, ImFont*> s_fonts; // by pixel size
	std::stack<int> s_pendingSizes;

	// adds the Font Awesome icons into the font just added to the atlas
	void MergeIconFont(ImFontAtlas& atlas, float size)
	{
		static const ImWchar kIconRange[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
		ImFontConfig merge{};
		merge.FontDataOwnedByAtlas = false;
		merge.MergeMode = true;
		merge.GlyphMinAdvanceX = 20.0f;
#if BOOST_OS_WINDOWS
		// embedded as an RCDATA resource of the executable
		const HMODULE module = GetModuleHandle(nullptr);
		const HRSRC resource = FindResource(module, MAKEINTRESOURCE(IDR_FONTAWESOME), RT_RCDATA);
		if (!resource)
			return;
		const HGLOBAL loaded = ::LoadResource(module, resource);
		if (!loaded)
			return;
		atlas.AddFontFromMemoryTTF(LockResource(loaded), (int)SizeofResource(module, resource), size, &merge, kIconRange);
#else
		atlas.AddFontFromMemoryTTF((void*)g_fontawesome_data, (int)g_fontawesome_size, size, &merge, kIconRange);
#endif
	}
}

void ImGui_PrecacheFonts()
{
	while (!s_pendingSizes.empty())
	{
		const int size = s_pendingSizes.top();
		s_pendingSizes.pop();

		ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
		cemu_assert(atlas.Locked == false);
		if (s_systemFontSize == 0)
			s_systemFont = extractCafeDefaultFont(&s_systemFontSize);

		ImFontConfig base{};
		base.FontDataOwnedByAtlas = false;
		ImFont* font = atlas.AddFontFromMemoryTTF(s_systemFont, s_systemFontSize, (float)size, &base);
		MergeIconFont(atlas, (float)size);
		s_fonts[size] = font;

		// the atlas texture changes; Vulkan must be done with the old one before it goes
		g_renderer->Flush(true);
		g_renderer->DeleteFontTextures();
	}
}

void ImGui_ClearFonts()
{
	s_fonts.clear();
}

ImFont* ImGui_GetFont(float size)
{
	const auto found = s_fonts.find((int)size);
	if (found != s_fonts.end())
		return found->second;
	s_pendingSizes.emplace((int)size);
	return nullptr;
}

// ---- input ----

void ImGui_UpdateWindowInformation(bool mainWindow)
{
	ImGuiIO& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	auto& windows = WindowSystem::GetWindowInfo();
#if BOOST_OS_WINDOWS
	io.ImeWindowHandle = mainWindow ? windows.window_main.surface : windows.window_pad.surface;
#else
	io.ImeWindowHandle = nullptr;
#endif

	// mouse: position in this window, left button only when held in this window
	auto& input = InputManager::instance();
	const auto mouse = input.get_mouse_position(!mainWindow);
	io.MousePos = {(float)mouse.x, (float)mouse.y};
	bool heldOnPad;
	const auto heldAt = input.get_left_down_mouse_info(&heldOnPad);
	io.MouseDown[0] = heldAt.has_value() && heldOnPad != mainWindow;

	// Platform key codes get imgui keys handed out in order of first use;
	// imgui only needs them to tell keys apart.
	static std::map<uint32, ImGuiKey> s_keyFor;
	static uint32 s_nextKey = 0;
	windows.iter_keystates([&io](auto&& state) {
		auto known = s_keyFor.find(state.first);
		if (known == s_keyFor.end())
		{
			known = s_keyFor.emplace(state.first, (ImGuiKey)(ImGuiKey_NamedKey_BEGIN + s_nextKey)).first;
			s_nextKey = (s_nextKey + 1) % (uint32)ImGuiKey_NamedKey_COUNT;
		}
		io.AddKeyEvent(known->second, state.second);
	});

	// any controller can navigate the overlay
	for (auto i = 0; i < InputManager::kMaxController; ++i)
	{
		const auto controller = input.get_controller(i);
		if (!controller)
			continue;
		const std::pair<bool, ImGuiNavInput> nav[] = {
			{controller->is_start_down(), ImGuiNavInput_Input},
			{controller->is_a_down(), ImGuiNavInput_Activate},
			{controller->is_b_down(), ImGuiNavInput_Cancel},
			{controller->is_left_down(), ImGuiNavInput_DpadLeft},
			{controller->is_right_down(), ImGuiNavInput_DpadRight},
			{controller->is_up_down(), ImGuiNavInput_DpadUp},
			{controller->is_down_down(), ImGuiNavInput_DpadDown},
		};
		for (const auto& [down, navInput] : nav)
		{
			if (down)
				io.NavInputs[navInput] = 1.0f;
		}
	}
}
