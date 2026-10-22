// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
// A two-axis stick mapped from four directions plus a "Modifier" that shortens its travel.
class AnalogStick : public ReshapableInput
{
public:
  using StateData = ReshapeData;

  AnalogStick(const char* name, std::unique_ptr<StickGate>&& stick_gate);
  AnalogStick(const char* name, const char* ui_name, std::unique_ptr<StickGate>&& stick_gate);

  ReshapeData GetReshapableState(bool adjusted) const final;
  ControlState GetGateRadiusAtAngle(double ang) const override;

  StateData GetState() const;
  StateData GetState(const InputOverrideFunction& override_func) const;

private:
  Control* GetModifierInput() const override;

  std::unique_ptr<StickGate> m_gate;
};

// An AnalogStick with an octagonal gate whose size and virtual notches are settings.
class OctagonAnalogStick : public AnalogStick
{
public:
  OctagonAnalogStick(const char* name, ControlState gate_radius);
  OctagonAnalogStick(const char* name, const char* ui_name, ControlState gate_radius);

  ControlState GetVirtualNotchSize() const override;
  ControlState GetGateRadiusAtAngle(double ang) const override;

private:
  SettingValue<double> m_notch_width;
  SettingValue<double> m_gate_percent;
};

}  // namespace ControllerEmu
