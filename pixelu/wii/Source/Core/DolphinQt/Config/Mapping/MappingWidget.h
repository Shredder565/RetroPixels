// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QString>
#include <QWidget>

class InputConfig;
class MappingButton;
class MappingIndicator;
class MappingNumeric;
class MappingWindow;
class QFormLayout;
class QPushButton;
class QGroupBox;

namespace ControllerEmu
{
class Control;
class ControlGroup;
class EmulatedController;
class NumericSettingBase;
enum class SettingVisibility;
}  // namespace ControllerEmu

// Base class of the pages (tabs) of a MappingWindow. A page lays out the control groups of
// one emulated controller as group boxes of mapping buttons, setting widgets and indicators.
class MappingWidget : public QWidget
{
  Q_OBJECT
public:
  explicit MappingWidget(MappingWindow* window);

  ControllerEmu::EmulatedController* GetController() const;

  MappingWindow* GetParent() const;

  virtual void LoadSettings() = 0;
  virtual void SaveSettings() = 0;
  virtual InputConfig* GetConfig() = 0;

signals:
  // Forwarded from the window: the indicator tick and "the configuration changed".
  void Update();
  void ConfigChanged();

protected:
  int GetPort() const;

  // A group box for `group`, titled with the group's own (translated) name or with `name`.
  QGroupBox* CreateGroupBox(ControllerEmu::ControlGroup* group);
  QGroupBox* CreateGroupBox(const QString& name, ControllerEmu::ControlGroup* group);
  // A group box with just the group's controls, spread over `columns` columns.
  QGroupBox* CreateControlsBox(const QString& name, ControllerEmu::ControlGroup* group,
                               int columns);
  void CreateControl(const ControllerEmu::Control* control, QFormLayout* layout, bool indicator);
  // The "..." button next to a setting, which edits the setting as an input expression.
  QPushButton* CreateSettingAdvancedMappingButton(ControllerEmu::NumericSettingBase& setting);

  void AddSettingWidget(QFormLayout* layout, ControllerEmu::NumericSettingBase* setting);
  void AddSettingWidgets(QFormLayout* layout, ControllerEmu::ControlGroup* group,
                         ControllerEmu::SettingVisibility visibility);

  void ShowAdvancedControlGroupDialog(ControllerEmu::ControlGroup* group);

private:
  MappingIndicator* MakeIndicator(ControllerEmu::ControlGroup* group);
  void AddEnabledToggle(QFormLayout* form, ControllerEmu::ControlGroup* group);
  void AddMousePointingButton(QFormLayout* form, ControllerEmu::ControlGroup* group);

  MappingWindow* const m_window;
  // The last input button created on this page; "iterative mapping" walks this chain.
  MappingButton* m_last_input_button = nullptr;
};
