// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

class QComboBox;
class QLabel;
class QPushButton;
class WiimoteEmuExtension;

namespace ControllerEmu
{
class Attachments;
}

// "General and Options" tab of an emulated Wii Remote: buttons, D-Pad, hotkeys, the
// extension selector, rumble and options.
class WiimoteEmuGeneral final : public MappingWidget
{
  Q_OBJECT
public:
  explicit WiimoteEmuGeneral(MappingWindow* window, WiimoteEmuExtension* extension);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
  ControllerEmu::Attachments* GetAttachments() const;

  // The combo box index changed, either by the user or by the config/expression.
  void OnAttachmentChanged(int index);
  // The user picked an entry in the combo box.
  void OnAttachmentSelected(int index);

  // Follows the attachment setting after a config change, and after each indicator tick.
  void SyncWithConfig();
  void SyncWithState();

  QComboBox* m_extension_combo = nullptr;
  QLabel* m_extension_dynamic_marker = nullptr;
  QPushButton* m_configure_extension = nullptr;
  WiimoteEmuExtension* const m_extension_page;
};
