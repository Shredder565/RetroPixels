// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

namespace Common
{
// Build identification, filled in from the generated scmrev.h. All strings are computed once.
const std::string& GetEmulatorName();
const std::string& GetScmDescStr();          // `git describe` output
const std::string& GetScmBranchStr();
const std::string& GetScmRevStr();           // name, branch (off master), build type, describe
const std::string& GetScmRevGitStr();        // full commit hash
const std::string& GetUserAgentStr();        // for HTTP requests
const std::string& GetScmDistributorStr();
const std::string& GetScmUpdateTrackStr();
const std::string& GetNetplayDolphinVer();   // compared between netplay peers
int GetScmCommitsAheadMaster();
}  // namespace Common
