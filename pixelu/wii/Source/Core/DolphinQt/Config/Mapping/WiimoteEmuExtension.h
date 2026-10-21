// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "DolphinQt/Config/Mapping/MappingWidget.h"

class QGroupBox;

// "Extension" tab of an emulated Wii Remote. It builds a frame for every extension type up
// front and shows only the one that is currently attached.
class WiimoteEmuExtension final : public MappingWidget
{
  Q_OBJECT
public:
  explicit WiimoteEmuExtension(MappingWindow* window);

  InputConfig* GetConfig() override;

  // Shows the frame of extension `type` (a WiimoteEmu::ExtensionNumber) and hides the rest.
  void ChangeExtensionType(u32 type);

private:
  void LoadSettings() override;
  void SaveSettings() override;

  QGroupBox* BuildClassicFrame();
  QGroupBox* BuildDrumsFrame();
  QGroupBox* BuildGuitarFrame();
  QGroupBox* BuildEmptyFrame();
  QGroupBox* BuildNunchukFrame();
  QGroupBox* BuildTurntableFrame();
  QGroupBox* BuildUDrawFrame();
  QGroupBox* BuildDrawsomeFrame();
  QGroupBox* BuildTaTaConFrame();
  QGroupBox* BuildShinkansenFrame();

  // Indexed by WiimoteEmu::ExtensionNumber.
  std::array<QGroupBox*, WiimoteEmu::ExtensionNumber::MAX> m_frames{};
};
