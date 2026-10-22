// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

namespace Common
{
// Build identification, filled in from the generated scmrev.h. All strings are computed once.
const std::string& GetEmulatorName();
const std::string& GetScmDescStr();          // PixelU-Wii's version (RetroPixels' version)
const std::string& GetScmBranchStr();
const std::string& GetScmRevStr();           // "PixelU-Wii", build type, version
const std::string& GetScmRevGitStr();        // full commit hash
const std::string& GetUserAgentStr();        // for HTTP requests: emulator name, compat version
const std::string& GetCompatDescStr();       // the Dolphin-style version servers are given
const std::string& GetScmDistributorStr();
const std::string& GetScmUpdateTrackStr();
const std::string& GetNetplayDolphinVer();   // compared between netplay peers
int GetScmCommitsAheadMaster();
}  // namespace Common
