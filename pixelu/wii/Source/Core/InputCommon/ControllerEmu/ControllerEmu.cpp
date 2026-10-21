// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControllerEmu.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "Common/IniFile.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace ControllerEmu
{
namespace
{
// One lock for all controllers: only one of them is normally polled at a time, so a lock per
// controller would not buy anything.
std::recursive_mutex s_state_mutex;
}  // namespace

// --- ControlGroupContainer ---------------------------------------------------------------------

ControlGroupContainer::~ControlGroupContainer() = default;

std::string ControlGroupContainer::GetDisplayName() const
{
  return GetName();
}

void ControlGroupContainer::LoadGroupsConfig(Common::IniFile::Section* sec,
                                             const std::string& base)
{
  for (const auto& group : groups)
    group->LoadConfig(sec, base);
}

void ControlGroupContainer::SaveGroupsConfig(Common::IniFile::Section* sec,
                                             const std::string& base)
{
  for (const auto& group : groups)
    group->SaveConfig(sec, base);
}

void ControlGroupContainer::UpdateGroupsReferences(
    ciface::ExpressionParser::ControlEnvironment& env)
{
  const auto lock = EmulatedController::GetStateLock();
  for (const auto& group : groups)
    group->UpdateReferences(env);
}

void ControlGroupContainer::SetInputOverrideFunction(InputOverrideFunction override_func)
{
  m_input_override_function = std::move(override_func);
}

void ControlGroupContainer::ClearInputOverrideFunction()
{
  m_input_override_function = InputOverrideFunction{};
}

// --- EmulatedController ------------------------------------------------------------------------

EmulatedController::~EmulatedController() = default;

std::unique_lock<std::recursive_mutex> EmulatedController::GetStateLock()
{
  return std::unique_lock<std::recursive_mutex>(s_state_mutex);
}

void EmulatedController::UpdateReferences(const ControllerInterface& devi)
{
  // Both locks: ours against readers of the references, the interface's against device changes.
  std::scoped_lock locks(s_state_mutex, devi.GetDevicesMutex());

  m_default_device_is_connected = devi.HasConnectedDevice(m_default_device);

  ciface::ExpressionParser::ControlEnvironment env(devi, GetDefaultDevice(), m_expression_vars);
  UpdateGroupsReferences(env);
  env.CleanUnusedVariables();
}

void EmulatedController::UpdateSingleControlReference(const ControllerInterface& devi,
                                                      ControlReference* ref)
{
  ciface::ExpressionParser::ControlEnvironment env(devi, GetDefaultDevice(), m_expression_vars);

  const auto lock = GetStateLock();
  ref->UpdateReference(env);
  env.CleanUnusedVariables();
}

const ciface::ExpressionParser::ControlEnvironment::VariableContainer&
EmulatedController::GetExpressionVariables() const
{
  return m_expression_vars;
}

void EmulatedController::ResetExpressionVariables()
{
  for (const auto& entry : m_expression_vars)
  {
    if (entry.second != nullptr)
      *entry.second = 0;
  }
}

bool EmulatedController::IsDefaultDeviceConnected() const
{
  return m_default_device_is_connected;
}

const ciface::Core::DeviceQualifier& EmulatedController::GetDefaultDevice() const
{
  return m_default_device;
}

void EmulatedController::SetDefaultDevice(const std::string& device)
{
  ciface::Core::DeviceQualifier qualifier;
  qualifier.FromString(device);
  SetDefaultDevice(std::move(qualifier));
}

void EmulatedController::SetDefaultDevice(ciface::Core::DeviceQualifier devq)
{
  m_default_device = std::move(devq);
}

void EmulatedController::LoadConfig(Common::IniFile::Section* sec)
{
  const auto lock = GetStateLock();

  // An absent or empty "Device" keeps the current default device.
  std::string device;
  if (sec->Get("Device", &device, "") && !device.empty())
    SetDefaultDevice(device);

  LoadGroupsConfig(sec, "");
}

void EmulatedController::SaveConfig(Common::IniFile::Section* sec)
{
  const auto lock = GetStateLock();

  sec->Set("Device", GetDefaultDevice().ToString(), "");
  SaveGroupsConfig(sec, "");
}

void EmulatedController::LoadDefaults(const ControllerInterface& ciface)
{
  const auto lock = GetStateLock();

  // Loading from an empty section resets every group to its defaults.
  Common::IniFile::Section empty;
  LoadConfig(&empty);

  const std::string& device = ciface.GetDefaultDeviceString();
  if (!device.empty())
    SetDefaultDevice(device);
}

}  // namespace ControllerEmu
