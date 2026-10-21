// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include <QDialog>

#include "Common/CommonTypes.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

class QCheckBox;
class QDialog;
class QEvent;
class QGridLayout;
class QGroupBox;
class QLayout;
class QSpinBox;
class QString;
class QWidget;
class TASCheckBox;
class TASSpinBox;

// Collects one override per (control group, control) pair and hands them to an emulated
// controller as a single input override function.
class InputOverrider final
{
public:
  using OverrideFunction = std::function<std::optional<ControlState>(ControlState)>;

  void AddFunction(std::string_view group_name, std::string_view control_name,
                   OverrideFunction function);

  ControllerEmu::InputOverrideFunction GetInputOverrideFunction() const;

private:
  using ControlKey = std::pair<std::string_view, std::string_view>;
  std::map<ControlKey, OverrideFunction> m_by_control;
};

// Shared base of the GameCube, Wii and GBA TAS input windows: the settings box (controller
// pass-through and turbo timing) and builders for buttons, sliders and sticks.
class TASInputWindow : public QDialog
{
  Q_OBJECT
public:
  explicit TASInputWindow(QWidget* parent);

  int GetTurboPressFrames() const;
  int GetTurboReleaseFrames() const;

protected:
  TASCheckBox* CreateButton(const QString& text, std::string_view group_name,
                            std::string_view control_name, InputOverrider* overrider);
  QGroupBox* CreateStickInputs(const QString& text, std::string_view group_name,
                               InputOverrider* overrider, int min_x, int min_y, int max_x,
                               int max_y, Qt::Key x_shortcut_key, Qt::Key y_shortcut_key);
  QGridLayout* CreateSliderValuePairLayout(const QString& text, std::string_view group_name,
                                           std::string_view control_name, InputOverrider* overrider,
                                           int zero, int default_, int min, int max,
                                           Qt::Key shortcut_key, QWidget* shortcut_widget,
                                           std::optional<ControlState> scale = {});
  TASSpinBox* CreateSliderValuePair(std::string_view group_name, std::string_view control_name,
                                    InputOverrider* overrider, QGridLayout* layout, int zero,
                                    int default_, int min, int max,
                                    const QKeySequence& shortcut_key_sequence,
                                    Qt::Orientation orientation, QWidget* shortcut_widget,
                                    std::optional<ControlState> scale = {});
  TASSpinBox* CreateSliderValuePair(QGridLayout* layout, int default_, int max,
                                    const QKeySequence& shortcut_key_sequence,
                                    Qt::Orientation orientation, QWidget* shortcut_widget);

  void SetupScrollArea(QLayout* layout);

  void changeEvent(QEvent* event) override;

  QWidget* m_scroll_widget;
  QGroupBox* m_settings_box;
  QCheckBox* m_use_controller;
  QSpinBox* m_turbo_press_frames;
  QSpinBox* m_turbo_release_frames;

private:
  // Called from the CPU thread with the real controller's value; returns the TAS value.
  std::optional<ControlState> PollButton(TASCheckBox* button, ControlState real_value);
  std::optional<ControlState> PollRanged(TASSpinBox* value, int zero, int min, int max,
                                         ControlState real_value);
  std::optional<ControlState> PollScaled(TASSpinBox* value, int zero, ControlState real_value,
                                         ControlState scale);
};
