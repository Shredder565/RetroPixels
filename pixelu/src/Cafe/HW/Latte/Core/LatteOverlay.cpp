// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/Account/Account.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "WindowSystem.h"

#include <imgui.h>
#include "resource/IconsFontAwesome5.h"
#include "imgui/imgui_extension.h"

#include "input/InputManager.h"
#include "util/SystemInfo/SystemInfo.h"

#include <cinttypes>

// The overlay draws two stacks of small ImGui panels into the TV or GamePad image: the
// statistics panel and the notification panels (account/controller profiles, friends,
// low battery, shader and pipeline compilation, and messages pushed by other code).
// Each stack starts at its configured screen corner or edge and grows away from it.

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;
extern std::atomic_int g_shaderStateCacheSetCount;
extern std::atomic_int g_shaderStateCacheSetAuxCount;

extern std::mutex g_friend_notification_mutex;
extern std::vector<std::pair<std::string, int>> g_friend_notifications;

// pipeline compilation counters, increased by the renderers
std::atomic_int g_compiling_pipelines;
std::atomic_int g_compiling_pipelines_async;
std::atomic_uint64_t g_compiling_pipelines_syncTimeSum; // nanoseconds spent compiling synchronously

namespace
{
	using Clock = std::chrono::steady_clock;

	struct
	{
		double fps = 0.0;
		uint32 drawCalls = 0;
		uint32 fastDrawCalls = 0;
		sint32 cpuCount = 1;
		ProcessorTime processTime; // this process, at the last update
		std::vector<ProcessorTime> coreTimes; // per core, at the last update
		float processCpuPercent = 0.0f;
		std::vector<float> coreCpuPercent;
		uint32 ramMB = 0;
		int vramUsedMB = 0;
		int vramTotalMB = 0;
	}s_stats;

	// messages from LatteOverlay_pushNotification, not yet shown
	std::mutex s_pendingMutex;
	std::vector<std::pair<std::string, int>> s_pending;

	constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	constexpr float kPanelAlpha = 0.65f;
	constexpr float kPanelGap = 10.0f;
	constexpr float kEdgeMargin = 10.0f;

	// where the next panel of a stack goes
	struct StackCursor
	{
		ImVec2 position{};
		ImVec2 pivot{};
		float step = 1.0f; // +1 grows downwards, -1 upwards
	};

	StackCursor CursorFor(ScreenPosition corner, const Vector2f& screen)
	{
		const float left = kEdgeMargin;
		const float centerX = screen.x / 2.0f;
		const float right = screen.x - kEdgeMargin;
		const float top = kEdgeMargin;
		const float bottom = screen.y - kEdgeMargin;
		switch (corner)
		{
		case ScreenPosition::kTopLeft: return { { left, top }, { 0.0f, 0.0f }, 1.0f };
		case ScreenPosition::kTopCenter: return { { centerX, top }, { 0.5f, 0.0f }, 1.0f };
		case ScreenPosition::kTopRight: return { { right, top }, { 1.0f, 0.0f }, 1.0f };
		case ScreenPosition::kBottomLeft: return { { left, bottom }, { 0.0f, 1.0f }, -1.0f };
		case ScreenPosition::kBottomCenter: return { { centerX, bottom }, { 0.5f, 1.0f }, -1.0f };
		case ScreenPosition::kBottomRight: return { { right, bottom }, { 1.0f, 1.0f }, -1.0f };
		default: UNREACHABLE;
		}
	}

	// draws one panel at the cursor; the body runs only if the panel is visible
	template<typename TBody>
	void Panel(StackCursor& cursor, const char* id, TBody&& body)
	{
		ImGui::SetNextWindowPos(cursor.position, ImGuiCond_Always, cursor.pivot);
		ImGui::SetNextWindowBgAlpha(kPanelAlpha);
		if (ImGui::Begin(id, nullptr, kPanelFlags))
		{
			body();
			cursor.position.y += (ImGui::GetWindowSize().y + kPanelGap) * cursor.step;
		}
		ImGui::End();
	}

	// pushes font and text color for one stack
	struct StackStyle
	{
		StackStyle(float fontSize, uint32 textColor)
		{
			ImGui::PushFont(ImGui_GetFont(fontSize));
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(textColor));
		}
		~StackStyle()
		{
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}
	};

	void IconLine(const char* icon)
	{
		ImGui::TextUnformatted(icon);
		ImGui::SameLine();
	}

	void SpinnerIcon(Clock::time_point now)
	{
		ImRotateStart();
		ImGui::TextUnformatted((const char*)ICON_FA_SPINNER);
		const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now).time_since_epoch().count();
		ImRotateEnd(0.001f * ms);
		ImGui::SameLine();
	}

	// ---- statistics ----

	void DrawStats(StackCursor& cursor, float fontSize)
	{
		const auto& cfg = GetConfig().overlay;
		StackStyle style(fontSize, cfg.text_color);
		const bool anyStat = cfg.fps || cfg.drawcalls || cfg.cpu_usage || cfg.cpu_per_core_usage || cfg.ram_usage;
		if (!anyStat)
			return;
		Panel(cursor, "##rp_stats", [&]
		{
			if (cfg.fps)
				ImGui::Text("FPS: %.2lf", s_stats.fps);
			if (cfg.drawcalls)
				ImGui::Text("Draws/f: %d (fast: %d)", s_stats.drawCalls, s_stats.fastDrawCalls);
			if (cfg.cpu_usage)
				ImGui::Text("CPU: %.2lf%%", s_stats.processCpuPercent);
			if (cfg.cpu_per_core_usage)
			{
				for (size_t core = 0; core < s_stats.coreCpuPercent.size(); core++)
					ImGui::Text("CPU #%d: %.2lf%%", (int)core + 1, s_stats.coreCpuPercent[core]);
			}
			if (cfg.ram_usage)
				ImGui::Text("RAM: %dMB", s_stats.ramMB);
			if (cfg.vram_usage && s_stats.vramUsedMB != -1 && s_stats.vramTotalMB != -1)
				ImGui::Text("VRAM: %dMB / %dMB", s_stats.vramUsedMB, s_stats.vramTotalMB);
			if (cfg.debug)
			{
				ImGui::Text("--- Debug info ---");
				ImGui::Text("IndexUploadPerFrame: %dKB", (performanceMonitor.stats.indexDataUploadPerFrame + 1023) / 1024);
				ImGui::Text("SHCSets: %d / %d", g_shaderStateCacheSetCount.load(), g_shaderStateCacheSetAuxCount.load());
				g_renderer->AppendOverlayDebugInfo();
			}
		});
	}

	// ---- notifications ----

	// account and controller profiles, shown during the first seconds of a session
	void DrawSessionInfo(StackCursor& cursor)
	{
		static bool s_done = false;
		if (s_done)
			return;
		static const Clock::time_point s_firstShown = tick_cached();
		if (tick_cached() - s_firstShown > std::chrono::milliseconds(5000))
		{
			s_done = true;
			return;
		}

		Panel(cursor, "##rp_account", []
		{
			IconLine((const char*)ICON_FA_USER);
			static std::string s_name;
			if (s_name.empty())
			{
				const std::wstring wide{ Account::GetAccount(ActiveSettings::GetPersistentId()).GetMiiName() };
				s_name = boost::nowide::narrow(wide);
			}
			ImGui::TextUnformatted(s_name.c_str());
		});

		std::vector<std::pair<int, std::string>> players;
		auto& input = InputManager::instance();
		for (int i = 0; i < InputManager::kMaxController; i++)
		{
			const auto controller = input.get_controller(i);
			if (controller && !controller->get_profile_name().empty())
				players.emplace_back(i, controller->get_profile_name());
		}
		if (players.empty())
		{
			s_done = true;
			return;
		}
		Panel(cursor, "##rp_profiles", [&]
		{
			for (size_t n = 0; n < players.size(); n++)
			{
				if (n > 0)
					ImGui::Separator();
				IconLine((const char*)ICON_FA_GAMEPAD);
				ImGui::Text("Player %d: %s", players[n].first + 1, players[n].second.c_str());
			}
		});
	}

	using TimedMessages = std::vector<std::pair<std::string, Clock::time_point>>;

	// moves new (text, milliseconds) entries into `shown` with their expiry time
	void AdoptMessages(std::vector<std::pair<std::string, int>>& incoming, TimedMessages& shown)
	{
		if (incoming.empty())
			return;
		const auto now = tick_cached();
		for (const auto& [text, ms] : incoming)
			shown.emplace_back(text, now + std::chrono::milliseconds(ms));
		incoming.clear();
	}

	// lists the messages; each is shown at least once and then dropped when expired
	void DrawMessages(StackCursor& cursor, const char* id, TimedMessages& messages)
	{
		if (messages.empty())
			return;
		Panel(cursor, id, [&]
		{
			const auto now = tick_cached();
			for (auto it = messages.begin(); it != messages.end();)
			{
				ImGui::TextUnformatted(it->first.c_str(), it->first.c_str() + it->first.size());
				it = now >= it->second ? messages.erase(it) : it + 1;
			}
		});
	}

	void DrawFriendMessages(StackCursor& cursor)
	{
		static TimedMessages s_shown;
		std::lock_guard lock(g_friend_notification_mutex);
		AdoptMessages(g_friend_notifications, s_shown);
		DrawMessages(cursor, "##rp_friends", s_shown);
	}

	void DrawLowBattery(StackCursor& cursor)
	{
		std::vector<int> players;
		auto& input = InputManager::instance();
		for (int i = 0; i < InputManager::kMaxController; i++)
		{
			const auto controller = input.get_controller(i);
			if (controller && controller->is_battery_low())
				players.emplace_back(i);
		}
		if (players.empty())
			return;

		// the icon alternates every 750ms
		static Clock::time_point s_lastBlink = tick_cached();
		static bool s_blinkOn = false;
		const auto now = tick_cached();
		if (now - s_lastBlink >= std::chrono::milliseconds(750))
		{
			s_blinkOn = !s_blinkOn;
			s_lastBlink = now;
		}
		const char* icon = (const char*)(s_blinkOn ? ICON_FA_BATTERY_EMPTY : ICON_FA_BATTERY_QUARTER);
		Panel(cursor, "##rp_battery", [&]
		{
			for (size_t n = 0; n < players.size(); n++)
			{
				if (n > 0)
					ImGui::Separator();
				IconLine(icon);
				ImGui::Text("Player %d", players[n] + 1);
			}
		});
	}

	// running count of compiled shaders or pipelines; resets 2.5s after the last new one
	struct CompileTally
	{
		int count = 0;
		int asyncCount = 0;
		bool started = false;
		Clock::time_point lastNew;

		// false if there is nothing to show (and nothing pending)
		bool Update(std::atomic_int& newCount, std::atomic_int& newAsync, Clock::time_point& now)
		{
			if (count <= 0 && newCount <= 0)
				return false;
			const int added = newCount.exchange(0);
			count += added;
			asyncCount += newAsync.exchange(0);
			if (!started)
			{
				lastNew = tick_cached();
				started = true;
			}
			now = tick_cached();
			if (added > 0)
				lastNew = now;
			if (now - lastNew >= std::chrono::milliseconds(2500))
			{
				count = 0;
				asyncCount = 0;
			}
			return count > 0;
		}
	};

	void DrawShaderCompiling(StackCursor& cursor)
	{
		static CompileTally s_tally;
		Clock::time_point now;
		if (!s_tally.Update(g_compiled_shaders_total, g_compiled_shaders_async, now))
			return;
		Panel(cursor, "##rp_shaders", [&]
		{
			SpinnerIcon(now);
			const char* noun = s_tally.count > 1 ? "shaders" : "shader";
			// async numbers only make sense to the user while async compilation is on
			if (s_tally.asyncCount > 0 && GetConfig().async_compile)
				ImGui::Text("Compiled %d new %s... (%d async)", s_tally.count, noun, s_tally.asyncCount);
			else
				ImGui::Text("Compiled %d new %s...", s_tally.count, noun);
		});
	}

	void DrawPipelineCompiling(StackCursor& cursor)
	{
		static CompileTally s_tally;
		Clock::time_point now;
		if (!s_tally.Update(g_compiling_pipelines, g_compiling_pipelines_async, now))
			return;
		Panel(cursor, "##rp_pipelines", [&]
		{
			SpinnerIcon(now);
			const char* noun = s_tally.count > 1 ? "pipelines" : "pipeline";
#ifdef CEMU_DEBUG_ASSERT
			const uint64 syncMs = g_compiling_pipelines_syncTimeSum / 1000000ull;
			if (s_tally.asyncCount > 0)
				ImGui::Text("Compiled %d new %s... (%d async) TotalSync: %" PRIu64 "ms", s_tally.count, noun, s_tally.asyncCount, syncMs);
			else
				ImGui::Text("Compiled %d new %s... TotalSync: %" PRIu64 "ms", s_tally.count, noun, syncMs);
#else
			if (s_tally.asyncCount > 0)
				ImGui::Text("Compiled %d new %s... (%d async)", s_tally.count, noun, s_tally.asyncCount);
			else
				ImGui::Text("Compiled %d new %s...", s_tally.count, noun);
#endif
		});
	}

	void DrawPushedMessages(StackCursor& cursor)
	{
		static TimedMessages s_shown;
		{
			std::lock_guard lock(s_pendingMutex);
			AdoptMessages(s_pending, s_shown);
		}
		DrawMessages(cursor, "##rp_messages", s_shown);
	}

	void DrawNotifications(StackCursor& cursor, float fontSize)
	{
		const auto& cfg = GetConfig().notification;
		StackStyle style(fontSize, cfg.text_color);
		if (cfg.controller_profiles)
			DrawSessionInfo(cursor);
		if (cfg.friends)
			DrawFriendMessages(cursor);
		if (cfg.controller_battery)
			DrawLowBattery(cursor);
		if (cfg.shader_compiling)
		{
			DrawShaderCompiling(cursor);
			DrawPipelineCompiling(cursor);
		}
		DrawPushedMessages(cursor);
	}

	// CPU usage since the last update, in percent
	void SampleCpuUsage()
	{
		ProcessorTime processNow;
		QueryProcTime(processNow);
		const double processShare = ProcessorTime::Compare(s_stats.processTime, processNow) / s_stats.cpuCount;
		s_stats.processCpuPercent = (float)(processShare * 100);
		s_stats.processTime = processNow;

		std::vector<ProcessorTime> coresNow(s_stats.cpuCount);
		QueryCoreTimes(s_stats.cpuCount, coresNow);
		for (size_t core = 0; core < s_stats.coreTimes.size() && core < coresNow.size(); core++)
		{
			s_stats.coreCpuPercent[core] = (float)(ProcessorTime::Compare(s_stats.coreTimes[core], coresNow[core]) * 100);
			s_stats.coreTimes[core] = coresNow[core];
		}
	}
}

void LatteOverlay_pushNotification(const std::string& text, sint32 duration)
{
	std::lock_guard lock(s_pendingMutex);
	s_pending.emplace_back(text, duration);
}

void LatteOverlay_render(bool pad_view)
{
	const auto& cfg = GetConfig();
	const ScreenPosition statsCorner = cfg.overlay.position;
	const ScreenPosition notifyCorner = cfg.notification.position;
	if (statsCorner == ScreenPosition::kDisabled && notifyCorner == ScreenPosition::kDisabled)
		return;

	sint32 w = 0, h = 0;
	if (pad_view && WindowSystem::IsPadWindowOpen())
		WindowSystem::GetPadWindowPhysSize(w, h);
	else
		WindowSystem::GetWindowPhysSize(w, h);
	if (w == 0 || h == 0)
		return;
	const Vector2f screen{ (float)w, (float)h };

	// font sizes scale with the configured text scale and the window DPI
	const float dpi = pad_view ? WindowSystem::GetPadDPIScale() : WindowSystem::GetWindowDPIScale();
	const float statsFont = 14.0f * (float)cfg.overlay.text_scale / 100.0f * dpi;
	const float notifyFont = 14.0f * (float)cfg.notification.text_scale / 100.0f * dpi;
	// nothing is drawn until both font sizes are available
	if (!ImGui_GetFont(statsFont) || !ImGui_GetFont(notifyFont))
		return;

	StackCursor cursor;
	if (statsCorner != ScreenPosition::kDisabled)
	{
		cursor = CursorFor(statsCorner, screen);
		DrawStats(cursor, statsFont);
	}
	if (notifyCorner != ScreenPosition::kDisabled)
	{
		// sharing a corner with the statistics continues their stack
		if (notifyCorner != statsCorner)
			cursor = CursorFor(notifyCorner, screen);
		DrawNotifications(cursor, notifyFont);
	}
}

void LatteOverlay_init()
{
	s_stats.cpuCount = GetProcessorCount();
	s_stats.coreTimes.resize(s_stats.cpuCount);
	s_stats.coreCpuPercent.resize(s_stats.cpuCount);
}

void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls)
{
	if (GetConfig().overlay.position == ScreenPosition::kDisabled)
		return;
	s_stats.fps = fps;
	s_stats.drawCalls = drawcalls;
	s_stats.fastDrawCalls = fastDrawcalls;
	SampleCpuUsage();
	s_stats.ramMB = (uint32)((QueryRamUsage() / 1000) / 1000);
	g_renderer->GetVRAMInfo(s_stats.vramUsedMB, s_stats.vramTotalMB);
}
