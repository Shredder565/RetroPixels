// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/GameProfile/GameProfile.h"
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"
#include "util/IniParser/IniParser.h"
#include "util/helpers/StringHelpers.h"
#include "Cafe/CafeSystem.h"

std::unique_ptr<GameProfile> g_current_game_profile = std::make_unique<GameProfile>();

namespace
{
	void LogBadValue(std::string_view value, std::string_view option)
	{
		cemuLog_log(LogType::Force, "Game profile: unsupported value '{}' for option '{}'", value, option);
	}

	void LogOutOfRange(std::string_view value, std::string_view option)
	{
		cemuLog_log(LogType::Force, "Game profile: value '{}' of option '{}' is out of range", value, option);
	}

	// "1"/"true" and "0"/"false", case-insensitive
	bool ReadBool(IniParser& ini, std::string_view name, bool& out)
	{
		std::optional<std::string_view> text = ini.FindOption(name);
		if (!text)
			return false;
		if (boost::iequals(*text, "1") || boost::iequals(*text, "true"))
			out = true;
		else if (boost::iequals(*text, "0") || boost::iequals(*text, "false"))
			out = false;
		else
		{
			LogBadValue(*text, name);
			return false;
		}
		return true;
	}

	void ReadBool(IniParser& ini, std::string_view name, std::optional<bool>& out)
	{
		bool value;
		if (ReadBool(ini, name, value))
			out = value;
	}

	// Enum values are accepted by number or by the name the enum's formatter prints
	template<typename TEnum>
	bool ReadEnum(IniParser& ini, std::string_view name, TEnum& out)
	{
		static_assert(std::is_enum_v<TEnum>);
		std::optional<std::string_view> text = ini.FindOption(name);
		if (!text)
			return false;
		for (TEnum candidate : TEnum())
		{
			if (boost::iequals(fmt::format("{}", fmt::underlying(candidate)), *text) || boost::iequals(fmt::format("{}", candidate), *text))
			{
				out = candidate;
				return true;
			}
		}
		return false;
	}

	template<typename TEnum>
	bool ReadEnum(IniParser& ini, std::string_view name, std::optional<TEnum>& out)
	{
		TEnum value;
		if (!ReadEnum(ini, name, value))
			return false;
		out = value;
		return true;
	}

	// first line "# <name>" of a profile names the game (at most 127 bytes are looked at)
	std::optional<std::string> ReadGameNameComment(const std::vector<uint8>& content)
	{
		if (content.empty() || content[0] != '#')
			return std::nullopt;
		const size_t limit = std::min<size_t>(content.size(), 128);
		size_t end = 1;
		while (end < limit && content[end] != '\n')
			end++;
		std::string name(content.begin() + 1, content.begin() + end);
		trim(name);
		return name;
	}

	template<typename T>
	void WriteSetting(FileStream* file, std::string_view key, const T& value)
	{
		file->writeLine(fmt::format("{} = {}", key, value).c_str());
	}

	template<typename T>
	void WriteSetting(FileStream* file, std::string_view key, const std::optional<T>& value)
	{
		if (value)
			WriteSetting(file, key, *value);
	}
}

void gameProfile_load()
{
	g_current_game_profile->ResetOptional();
	g_current_game_profile->Load(CafeSystem::GetForegroundTitleId());

	ppcThreadQuantum = g_current_game_profile->GetThreadQuantum();
	if (ppcThreadQuantum != GameProfile::kThreadQuantumDefault)
		cemuLog_log(LogType::Force, "Thread quantum set to {}", ppcThreadQuantum);
}

bool GameProfile::Load(uint64_t title_id)
{
	// a profile in the user's config folder wins over the bundled default one
	fs::path path = ActiveSettings::GetConfigPath("gameProfiles/{:016x}.ini", title_id);
	std::optional<std::vector<uint8>> content = FileStream::LoadIntoMemory(path);
	bool isBundledDefault = false;
	if (!content)
	{
		path = ActiveSettings::GetDataPath("gameProfiles/default/{:016x}.ini", title_id);
		content = FileStream::LoadIntoMemory(path);
		if (!content)
			return false; // nothing loaded, state unchanged
		isBundledDefault = true;
	}
	m_is_default = isBundledDefault;
	m_is_loaded = true;

	if (std::optional<std::string> name = ReadGameNameComment(*content))
		m_gameName = std::move(name);

	IniParser ini(*content, _pathToUtf8(path));
	while (ini.NextSection())
	{
		const std::string_view section = ini.GetCurrentSectionName();
		if (boost::iequals(section, "General"))
		{
			ReadBool(ini, "loadSharedLibraries", m_loadSharedLibraries);
			ReadBool(ini, "startWithPadView", m_startWithPadView);
		}
		else if (boost::iequals(section, "Graphics"))
		{
			if (std::optional<std::string_view> api = ini.FindOption("graphics_api"))
			{
				const sint32 value = StringHelpers::ToInt(*api, -1);
				if (value == 0 || value == 1)
					m_graphics_api = (GraphicAPI)value;
				else
					LogOutOfRange(*api, "graphics_api");
			}
			ReadEnum(ini, "accurateShaderMul", m_accurateShaderMul);
			// boolean in older profiles; anything else (or no entry) means Auto
			m_precompiledShaders = PrecompiledShaderOption::Auto;
			if (std::optional<std::string_view> precompiled = ini.FindOption("precompiledShaders"))
			{
				if (boost::iequals(*precompiled, "1") || boost::iequals(*precompiled, "true"))
					m_precompiledShaders = PrecompiledShaderOption::Enable;
				else if (boost::iequals(*precompiled, "0") || boost::iequals(*precompiled, "false"))
					m_precompiledShaders = PrecompiledShaderOption::Disable;
			}
		}
		else if (boost::iequals(section, "Audio"))
		{
			ReadBool(ini, "disableAudio", m_disableAudio);
		}
		else if (boost::iequals(section, "CPU"))
		{
			if (std::optional<std::string_view> quantum = ini.FindOption("threadQuantum"))
			{
				const uint32 value = ConvertString<uint32>(*quantum);
				if (value >= 1000 && value <= 0x20000000)
					m_threadQuantum = value;
				else
					LogOutOfRange(*quantum, "threadQuantum");
			}
			if (!ReadEnum(ini, "cpuMode", m_cpuMode))
			{
				// profiles written by older versions use the legacy mode names
				CPUModeLegacy legacy;
				if (ReadEnum(ini, "cpuMode", legacy))
				{
					m_cpuMode = (CPUMode)legacy;
					if (m_cpuMode == CPUMode::DualcoreRecompiler)
						m_cpuMode = CPUMode::MulticoreRecompiler;
				}
			}
		}
		else if (boost::iequals(section, "Controller"))
		{
			for (size_t i = 0; i < m_controllerProfile.size(); i++)
			{
				if (std::optional<std::string_view> profile = ini.FindOption(fmt::format("controller{}", i + 1)))
					m_controllerProfile[i] = std::string(*profile);
			}
		}
	}
	return true;
}

void GameProfile::Save(uint64_t title_id)
{
	const fs::path folder = ActiveSettings::GetConfigPath("gameProfiles");
	std::error_code ec;
	if (!fs::exists(folder, ec))
		fs::create_directories(folder, ec);
	std::unique_ptr<FileStream> file(FileStream::createFile2(folder / fmt::format("{:016x}.ini", title_id)));
	if (!file)
	{
		cemuLog_log(LogType::Force, "Game profile: could not write the profile file");
		return;
	}

	if (m_gameName)
		file->writeLine(fmt::format("# {}\n", *m_gameName).c_str());

	file->writeLine("[General]");
	WriteSetting(file.get(), "loadSharedLibraries", m_loadSharedLibraries);
	WriteSetting(file.get(), "startWithPadView", m_startWithPadView);
	file->writeLine("");

	file->writeLine("[CPU]");
	WriteSetting(file.get(), "cpuMode", m_cpuMode);
	WriteSetting(file.get(), "threadQuantum", m_threadQuantum);
	file->writeLine("");

	file->writeLine("[Graphics]");
	WriteSetting(file.get(), "accurateShaderMul", m_accurateShaderMul);
	WriteSetting(file.get(), "precompiledShaders", m_precompiledShaders);
	WriteSetting(file.get(), "graphics_api", m_graphics_api);
	file->writeLine("");

	file->writeLine("[Controller]");
	for (size_t i = 0; i < m_controllerProfile.size(); i++)
		WriteSetting(file.get(), fmt::format("controller{}", i + 1), m_controllerProfile[i]);
	file->writeLine("");
}

void GameProfile::ResetOptional()
{
	m_gameName.reset();
	m_loadSharedLibraries.reset();
	m_startWithPadView = false;
	m_accurateShaderMul = AccurateShaderMulOption::True;
	// graphics API and precompiled shader choice are left as they are
	m_threadQuantum = kThreadQuantumDefault;
	m_cpuMode.reset();
	m_disableAudio = false;
	for (std::optional<std::string>& profile : m_controllerProfile)
		profile.reset();
}

void GameProfile::Reset()
{
	m_gameName.reset();
	m_loadSharedLibraries = true;
	m_startWithPadView = false;
	m_accurateShaderMul = AccurateShaderMulOption::True;
	m_precompiledShaders = PrecompiledShaderOption::Auto;
	// graphics API is left as it is
	m_threadQuantum = kThreadQuantumDefault;
	m_cpuMode = CPUMode::Auto;
	m_disableAudio = false;
	for (std::optional<std::string>& profile : m_controllerProfile)
		profile.reset();
}
