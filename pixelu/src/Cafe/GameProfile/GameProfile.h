// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "config/CemuConfig.h"

// integer setting as edited by the game profile window
struct gameProfileIntegerOption_t
{
	bool isPresent = false;
	sint32 value;
};

// Per-title settings, read from gameProfiles/<titleid>.ini in the config folder
// (user profile) or the data folder (bundled default profile). Unset optional
// values mean "use the global setting".
class GameProfile
{
	friend class GameProfileWindow; // edits the members below directly

  public:
	static const uint32 kThreadQuantumDefault = 45000;

	bool Load(uint64_t title_id); // false if no profile exists for the title
	void Save(uint64_t title_id); // always writes the user profile
	void ResetOptional();         // clears everything back to "use global setting"
	void Reset();                 // sets every option to an explicit default

	[[nodiscard]] uint64 GetTitleId() const { return m_title_id; }
	[[nodiscard]] bool IsLoaded() const { return m_is_loaded; }
	[[nodiscard]] bool IsDefaultProfile() const { return m_is_default; }
	[[nodiscard]] const std::optional<std::string>& GetGameName() const { return m_gameName; }

	[[nodiscard]] const std::optional<bool>& ShouldLoadSharedLibraries() const { return m_loadSharedLibraries; }
	[[nodiscard]] bool StartWithGamepadView() const { return m_startWithPadView; }

	[[nodiscard]] const std::optional<GraphicAPI>& GetGraphicsAPI() const { return m_graphics_api; }
	[[nodiscard]] const AccurateShaderMulOption& GetAccurateShaderMul() const { return m_accurateShaderMul; }
	[[nodiscard]] const std::optional<PrecompiledShaderOption>& GetPrecompiledShadersState() const { return m_precompiledShaders; }

	[[nodiscard]] uint32 GetThreadQuantum() const { return m_threadQuantum; }
	[[nodiscard]] const std::optional<CPUMode>& GetCPUMode() const { return m_cpuMode; }

	[[nodiscard]] bool IsAudioDisabled() const { return m_disableAudio; }

	[[nodiscard]] const std::array<std::optional<std::string>, 8>& GetControllerProfile() const { return m_controllerProfile; }

  private:
	uint64_t m_title_id = 0;
	bool m_is_loaded = false;
	bool m_is_default = true;

	std::optional<std::string> m_gameName{};

	// [General]
	std::optional<bool> m_loadSharedLibraries{};
	bool m_startWithPadView = false;

	// [Graphics]
	std::optional<GraphicAPI> m_graphics_api{};
	AccurateShaderMulOption m_accurateShaderMul = AccurateShaderMulOption::True;
	std::optional<PrecompiledShaderOption> m_precompiledShaders{};

	// [CPU]
	uint32 m_threadQuantum = kThreadQuantumDefault;
	std::optional<CPUMode> m_cpuMode{};

	// [Audio]
	bool m_disableAudio = false;

	// [Controller] controller1 ... controller8
	std::array<std::optional<std::string>, 8> m_controllerProfile{};
};

extern std::unique_ptr<GameProfile> g_current_game_profile;

// loads the profile of the foreground title into g_current_game_profile
void gameProfile_load();
