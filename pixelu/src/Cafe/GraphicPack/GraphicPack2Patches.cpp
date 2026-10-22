// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Common/FileStream.h"
#include "WindowSystem.h"
#include "util/helpers/StringParser.h"
#include "Cemu/PPCAssembler/ppcAssembler.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "boost/algorithm/string.hpp"

std::recursive_mutex GraphicPack2::s_patchMutex;
std::vector<const RPLModule*> GraphicPack2::s_loadedModules;

// ---------------------------------------------------------------- error reporting

void PatchErrorHandler::printError(class PatchGroup* patchGroup, sint32 lineNumber, std::string_view errorMsg)
{
	if (!m_anyErrorTriggered)
	{
		cemu_assert(m_gp);
		const char* what = m_stage == STAGE::PARSER ? "parsing" : "applying";
		cemuLog_writeLineToLog(fmt::format("Graphic pack '{}': error while {} patches", m_gp->GetName(), what), true, true);
	}
	m_anyErrorTriggered = true;

	std::string location;
	if (lineNumber >= 0)
		location = patchGroup ? fmt::format("[{}, Line {}] ", patchGroup->getName(), lineNumber) : fmt::format("[Line {}] ", lineNumber);
	else if (patchGroup)
		location = fmt::format("[{}] ", patchGroup->getName());
	std::string line = location + std::string(errorMsg);

	cemuLog_writeLineToLog(line, true, true);
	if (cemuLog_isLoggingEnabled(LogType::Patches))
		errorMessages.push_back(std::move(line));
}

void PatchErrorHandler::showStageErrorMessageBox()
{
	cemu_assert_debug(m_gp != nullptr);
	std::string text;
	if (m_gp)
	{
		if (m_stage == STAGE::PARSER)
			text = _tr("Failed to load patches for graphic pack \'{}\'", m_gp->GetName());
		else
			text = _tr("Failed to apply patches for graphic pack \'{}\'", m_gp->GetName());
	}
	if (cemuLog_isLoggingEnabled(LogType::Patches))
	{
		text += "\n \n";
		text += _tr("Details:");
		text += "\n";
		for (const std::string& msg : errorMessages)
		{
			text += msg;
			text += "\n";
		}
	}
	WindowSystem::ShowErrorDialog(text, _tr("Graphic pack error"), WindowSystem::ErrorCategory::GRAPHIC_PACKS);
}

// ---------------------------------------------------------------- loading patch files

// patch_*.asm files (Cemu format); returns true if at least one such file exists, even when it fails to parse
bool GraphicPack2::LoadCemuPatches()
{
	bool anyFile = false;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(m_rulesPath.parent_path(), ec))
	{
		const fs::path& file = entry.path();
		if (!entry.is_regular_file(ec) || !file.has_filename())
			continue;
		const std::string fileName = _pathToUtf8(file.filename());
		if (!boost::istarts_with(fileName, "patch_") || !boost::iends_with(fileName, ".asm"))
			continue;
		anyFile = true;
		std::unique_ptr<FileStream> stream(FileStream::openFile2(file));
		if (!stream)
		{
			cemuLog_log(LogType::Force, "Graphic pack: cannot open patch file {}", _pathToUtf8(file));
			continue;
		}
		std::vector<uint8> text;
		stream->extract(text);
		MemStreamReader reader(text.data(), (sint32)text.size());
		if (!ParseCemuPatchesTxtInternal(reader))
		{
			cemuLog_log(LogType::Force, "Graphic pack: {} has errors, none of the pack's patches are used", _pathToUtf8(file));
			return true;
		}
	}
	return anyFile;
}

void GraphicPack2::LoadPatchFiles()
{
	// patch_*.asm files take precedence; patches.txt (Cemuhook format) is only read when there are none
	if (LoadCemuPatches())
		return;
	std::unique_ptr<FileStream> stream(FileStream::openFile2(m_rulesPath.parent_path() / "patches.txt"));
	if (!stream)
		return;
	std::vector<uint8> text;
	stream->extract(text);
	MemStreamReader reader(text.data(), (sint32)text.size());
	ParseCemuhookPatchesTxtInternal(reader);
}

void GraphicPack2::DropPatchGroups()
{
	for (PatchGroup* group : m_patchGroups)
		delete group;
	m_patchGroups.clear();
}

// ---------------------------------------------------------------- applying to modules

std::vector<PatchGroup*> GraphicPack2::GroupsForModule(const RPLModule* rpl) const
{
	std::vector<PatchGroup*> matching;
	for (PatchGroup* group : m_patchGroups)
	{
		if (group->matchesCRC(rpl->patchCRC) || (group->m_targetsRpx && rpl->IsRPX()))
			matching.push_back(group);
	}
	return matching;
}

void GraphicPack2::ApplyPatchesForModule(const RPLModule* rpl)
{
	auto groups = GroupsForModule(rpl);
	if (!groups.empty())
		ApplyPatchGroups(groups, rpl);
}

void GraphicPack2::RevertPatchesForModule(const RPLModule* rpl)
{
	auto groups = GroupsForModule(rpl);
	if (!groups.empty())
		UndoPatchGroups(groups, rpl);
}

void GraphicPack2::EnablePatches()
{
	std::lock_guard lock(s_patchMutex);
	for (const RPLModule* module : s_loadedModules)
		ApplyPatchesForModule(module);
}

void GraphicPack2::UnloadPatches()
{
	if (m_patchGroups.empty())
		return;
	std::lock_guard lock(s_patchMutex);
	for (const RPLModule* module : s_loadedModules)
		RevertPatchesForModule(module);
	DropPatchGroups();
	m_callbacks.clear();
}

bool GraphicPack2::HasPatches()
{
	return !m_patchGroups.empty();
}

const std::vector<PatchGroup*>& GraphicPack2::GetPatchGroups()
{
	return m_patchGroups;
}

void GraphicPack2::NotifyModuleLoaded(const RPLModule* rpl)
{
	cemuLog_log(LogType::Force, "Module '{}' loaded, patch checksum 0x{:08x}", rpl->moduleName, rpl->patchCRC);
	std::lock_guard lock(s_patchMutex);
	s_loadedModules.push_back(rpl);
}

void GraphicPack2::NotifyModuleUnloaded(const RPLModule* rpl)
{
	std::lock_guard lock(s_patchMutex);
	std::erase(s_loadedModules, rpl);
}
