// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QString>

#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

class MappingWidget;

// Editors for the numeric settings of a control group. A setting may hold a plain value or
// an input expression; in the latter case the editor follows the live value and is marked
// with a controller symbol.

// Note: these classes have no Q_OBJECT, so their tr() texts use the context of the Qt base
// class (QDoubleSpinBox and QCheckBox). Existing translations depend on that.
class MappingDouble : public QDoubleSpinBox
{
public:
  MappingDouble(MappingWidget* parent, ControllerEmu::NumericSetting<double>* setting);

private:
  // An emptied field falls back to the setting's default value.
  void fixup(QString& input) const override;

  void ShowSetting();
  void FollowLiveValue();

  ControllerEmu::NumericSetting<double>& m_setting;
};

class MappingBool : public QCheckBox
{
public:
  MappingBool(MappingWidget* widget, ControllerEmu::NumericSetting<bool>* setting);

private:
  void ShowSetting();
  void FollowLiveValue();

  ControllerEmu::NumericSetting<bool>& m_setting;
};
