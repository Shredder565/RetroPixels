// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include <mutex>

#include "KeyCache.h"
#include "Cemu/Logging/CemuLogging.h"
#include "WindowSystem.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"
#include "util/helpers/StringHelpers.h"

// keys.txt: one 32 digit hex key per line. Text after '#' or ';' is a
// comment; spaces, tabs, '-' and '_' inside a key are ignored. Lines with
// other characters are reported, lines of another length are skipped.
namespace
{
	std::mutex s_keysMutex;
	bool s_keysLoaded = false;
	std::vector<std::array<uint8, 16>> s_keys;

	constexpr const char* kNewKeysFile =
		"# this file contains keys needed for decryption of disc file system data (WUD/WUX)\r\n"
		"# 1 key per line, any text after a '#' character is considered a comment\r\n"
		"# the emulator will automatically pick the right key\r\n"
		"541b9889519b27d363cd21604b97c67a # example key (can be deleted)\r\n";

	// strips comments and separator characters
	std::string CleanLine(const std::string& line)
	{
		std::string out;
		for (char c : line)
		{
			if (c == '#' || c == ';')
				break;
			if (c == ' ' || c == '\t' || c == '-' || c == '_')
				continue;
			out.push_back(c);
		}
		return out;
	}

	bool IsHexDigits(std::string_view text)
	{
		return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
	}

	void CreateKeysFile(const fs::path& path)
	{
		std::unique_ptr<FileStream> file(FileStream::createFile2(path));
		if (!file)
		{
			WindowSystem::ShowErrorDialog(_tr("Unable to create file keys.txt\nThis can happen if PixelU does not have write permission to its own directory, the disk is full or if anti-virus software is blocking PixelU."), _tr("Error"), WindowSystem::ErrorCategory::KEYS_TXT_CREATION);
			return;
		}
		file->writeString(kNewKeysFile);
	}
}

uint8* KeyCache_GetAES128(sint32 index)
{
	if (index < 0 || (size_t)index >= s_keys.size())
		return nullptr;
	return s_keys[index].data();
}

void KeyCache_Prepare()
{
	std::lock_guard lock(s_keysMutex);
	if (s_keysLoaded)
		return;
	s_keysLoaded = true;
	s_keys.clear();
	const fs::path keysPath = ActiveSettings::GetUserDataPath("keys.txt");
	std::unique_ptr<FileStream> file(FileStream::openFile2(keysPath));
	if (!file)
	{
		CreateKeysFile(keysPath);
		return;
	}
	std::string line;
	for (sint32 lineNumber = 1; file->readLine(line); lineNumber++)
	{
		const std::string key = CleanLine(line);
		if (key.empty())
			continue;
		if (!IsHexDigits(key))
		{
			WindowSystem::ShowErrorDialog(_tr("Error in keys.txt at line {}", lineNumber), WindowSystem::ErrorCategory::KEYS_TXT_CREATION);
			continue;
		}
		if (key.size() != 32)
			continue;
		std::array<uint8, 16> parsed;
		StringHelpers::ParseHexString(key, parsed.data(), 16);
		s_keys.push_back(parsed);
	}
}
