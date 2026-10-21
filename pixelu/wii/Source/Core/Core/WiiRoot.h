// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <vector>

#include "DiscIO/RiivolutionPatcher.h"

class BootSessionData;

namespace IOS::HLE::FS
{
struct NandRedirect;
}

// Choosing the NAND root an emulation session uses, and preparing and tearing
// down its contents.
namespace Core
{
enum class RestoreReason
{
  EmulationEnd,
  CrashRecovery,
};

// Selects the session NAND: the user's own, or (for movies and NetPlay) a
// throwaway one under the user folder.
void InitializeWiiRoot(bool use_temporary);
void ShutdownWiiRoot();

bool WiiRootIsInitialized();
bool WiiRootIsTemporary();

// Saves copies of NAND files that boot may rewrite, and puts them back later.
void BackupWiiSettings();
void RestoreWiiSettings(RestoreReason reason);

// Fills the session NAND (system files, saves, redirects) at boot, and copies
// results back to the user's NAND at the end where appropriate.
void InitializeWiiFileSystemContents(
    std::optional<DiscIO::Riivolution::SavegameRedirect> save_redirect,
    const BootSessionData& boot_session_data);
void CleanUpWiiFileSystemContents(const BootSessionData& boot_session_data);

const std::vector<IOS::HLE::FS::NandRedirect>& GetActiveNandRedirects();
}  // namespace Core
