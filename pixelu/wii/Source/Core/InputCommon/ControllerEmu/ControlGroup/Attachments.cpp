// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"

#include <cstddef>
#include <utility>

#include "Common/StringUtil.h"

namespace ControllerEmu
{
void AttachedController::LoadDefaults()
{
}

Attachments::Attachments(const std::string& name_) : ControlGroup(name_, GroupType::Attachments)
{
}

void Attachments::AddAttachment(std::unique_ptr<AttachedController> att)
{
  m_attachments.push_back(std::move(att));
}

u32 Attachments::GetSelectedAttachment() const
{
  const int index = m_selection.GetValue();
  if (index <= 0 || static_cast<std::size_t>(index) >= m_attachments.size())
    return 0;
  return static_cast<u32>(index);
}

void Attachments::SetSelectedAttachment(u32 val)
{
  m_selection_setting.SetValue(static_cast<int>(val));
}

NumericSetting<int>& Attachments::GetSelectionSetting()
{
  return m_selection_setting;
}

SubscribableSettingValue<int>& Attachments::GetAttachmentSetting()
{
  return m_selection;
}

const std::vector<std::unique_ptr<AttachedController>>& Attachments::GetAttachmentList() const
{
  return m_attachments;
}

void Attachments::LoadConfig(Common::IniFile::Section* sec, const std::string& base)
{
  ControlGroup::LoadConfig(sec, base);

  SetSelectedAttachment(0);

  std::string selection;
  sec->Get(base + name, &selection, "");

  // The text is taken as an expression unless it turns out to be an attachment's name.
  m_selection_setting.GetInputReference().SetExpression(selection);

  for (u32 index = 0; index != m_attachments.size(); ++index)
  {
    AttachedController& attachment = *m_attachments[index];
    attachment.LoadGroupsConfig(sec, base + attachment.GetName() + "/");
    if (attachment.GetName() == selection)
      SetSelectedAttachment(index);
  }
}

void Attachments::SaveConfig(Common::IniFile::Section* sec, const std::string& base)
{
  ControlGroup::SaveConfig(sec, base);

  std::string selection;
  if (m_selection_setting.IsSimpleValue())
  {
    selection = m_attachments[GetSelectedAttachment()]->GetName();
  }
  else
  {
    selection = m_selection_setting.GetInputReference().GetExpression();
    ReplaceBreaksWithSpaces(selection);
  }
  sec->Set(base + name, selection, "None");

  for (const auto& attachment : m_attachments)
    attachment->SaveGroupsConfig(sec, base + attachment->GetName() + "/");
}

void Attachments::UpdateReferences(ciface::ExpressionParser::ControlEnvironment& env)
{
  ControlGroup::UpdateReferences(env);

  m_selection_setting.GetInputReference().UpdateReference(env);

  for (const auto& attachment : m_attachments)
    attachment->UpdateGroupsReferences(env);
}

}  // namespace ControllerEmu
