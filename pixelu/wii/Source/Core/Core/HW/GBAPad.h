// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

class InputConfig;
enum class GBAPadGroup;
struct GCPadStatus;

namespace ControllerEmu
{
class ControlGroup;
}  // namespace ControllerEmu

// Button mappings for the four emulated GBAs (link cable or GB Player).
namespace Pad
{
void ShutdownGBA();
void InitializeGBA();
void LoadGBAConfig();
bool IsGBAInitialized();

InputConfig* GetGBAConfig();

GCPadStatus GetGBAStatus(int pad_num);
void SetGBAReset(int pad_num, bool reset);

ControllerEmu::ControlGroup* GetGBAGroup(int pad_num, GBAPadGroup group);
}  // namespace Pad
