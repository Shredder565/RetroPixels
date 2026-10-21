// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

// The updater's progress window and host helpers, supplied by each updater program.
namespace UI
{
void Error(const std::string& text);

void SetDescription(const std::string& text);

// The window has an overall progress bar and one for the current file; "marquee" makes a bar
// show activity without a known amount.
void SetTotalMarquee(bool marquee);
void ResetTotalProgress();
void SetTotalProgress(int current, int total);

void SetCurrentMarquee(bool marquee);
void ResetCurrentProgress();
void SetCurrentProgress(int current, int total);

void SetVisible(bool visible);

void Stop();

void Init();
void Sleep(int seconds);
void WaitForPID(u32 pid);
void LaunchApplication(std::string path);

// True when running against the test update server (manifests signed with the test key).
bool IsTestMode();
}  // namespace UI
