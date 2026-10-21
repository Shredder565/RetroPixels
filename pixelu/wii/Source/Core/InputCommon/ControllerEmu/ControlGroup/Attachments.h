// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
// Something that can be plugged into a controller (a Wii Remote extension).
class AttachedController : public ControlGroupContainer
{
public:
  virtual void LoadDefaults();
};

// The list of possible attachments and the selected one. The selection is saved by the
// attachment's name (or as an expression), and each attachment's groups are saved under
// "<attachment name>/".
class Attachments : public ControlGroup
{
public:
  explicit Attachments(const std::string& name);

  void AddAttachment(std::unique_ptr<AttachedController> att);

  // Index into the attachment list; out-of-range selections read as 0.
  u32 GetSelectedAttachment() const;
  void SetSelectedAttachment(u32 val);

  NumericSetting<int>& GetSelectionSetting();
  SubscribableSettingValue<int>& GetAttachmentSetting();

  const std::vector<std::unique_ptr<AttachedController>>& GetAttachmentList() const;

  void LoadConfig(Common::IniFile::Section* sec, const std::string& base) override;
  void SaveConfig(Common::IniFile::Section* sec, const std::string& base) override;

  void UpdateReferences(ciface::ExpressionParser::ControlEnvironment& env) override;

private:
  // Declared before the setting, which writes its default into it on construction.
  SubscribableSettingValue<int> m_selection;
  // Not part of numeric_settings: it is saved by name rather than by number and the UI shows it
  // as a list. Otherwise it behaves like the group's other settings.
  NumericSetting<int> m_selection_setting = {&m_selection, {""}, 0, 0, 0};

  std::vector<std::unique_ptr<AttachedController>> m_attachments;
};
}  // namespace ControllerEmu
