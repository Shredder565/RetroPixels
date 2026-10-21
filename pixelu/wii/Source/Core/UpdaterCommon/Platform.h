// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <optional>
#include <sstream>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

#include "UpdaterCommon/UpdaterCommon.h"

// Supplied by each updater program.
namespace Platform
{
// Checks that this system can run the build being installed (and prepares it if needed), given
// the files about to be updated and the folder their new contents were downloaded to.
bool VersionCheck(const std::vector<TodoList::UpdateOp>& to_update,
                  const std::string& install_base_path, const std::string& temp_dir);
}  // namespace Platform
