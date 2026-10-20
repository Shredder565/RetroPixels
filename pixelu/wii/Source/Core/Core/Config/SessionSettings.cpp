// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/SessionSettings.h"

#include "Common/CPUDetect.h"

namespace Config
{
// Session settings are never written to an INI file; they only exist for the current run
// (set by NetPlay, movies or the boot code).
namespace
{
Location SessionKey(std::string_view key)
{
  return Location{System::Session, "Core", std::string(key)};
}
}  // namespace

// FMA is used when the host CPU has it, unless a movie or NetPlay session says otherwise.
const Info<bool> SESSION_USE_FMA{SessionKey("UseFMA"), CPUInfo().bFMA};
const Info<bool> SESSION_LOAD_IPL_DUMP{SessionKey("LoadIPLDump"), true};
const Info<bool> SESSION_GCI_FOLDER_CURRENT_GAME_ONLY{SessionKey("GCIFolderCurrentGameOnly"),
                                                      false};
const Info<bool> SESSION_CODE_SYNC_OVERRIDE{SessionKey("CheatSyncOverride"), false};
const Info<bool> SESSION_SAVE_DATA_WRITABLE{SessionKey("SaveDataWritable"), true};
const Info<bool> SESSION_SHOULD_FAKE_ERROR_001{SessionKey("ShouldFakeError001"), false};
}  // namespace Config
