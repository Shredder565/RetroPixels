// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/MappingWidget.h"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/core.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "DolphinQt/Config/Mapping/IOWindow.h"
#include "DolphinQt/Config/Mapping/MappingButton.h"
#include "DolphinQt/Config/Mapping/MappingIndicator.h"
#include "DolphinQt/Config/Mapping/MappingNumeric.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/MixedTriggers.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace
{
using ControllerEmu::GroupType;

// Group types whose indicator draws a reshapable input that can be calibrated.
bool HasCalibration(GroupType type)
{
  switch (type)
  {
  case GroupType::Cursor:
  case GroupType::Stick:
  case GroupType::Tilt:
  case GroupType::Force:
    return true;
  default:
    return false;
  }
}
}  // namespace

MappingWidget::MappingWidget(MappingWindow* window) : m_window(window)
{
  // The window drives every page: indicator ticks, saving, and config refreshes.
  connect(window, &MappingWindow::Update, this, &MappingWidget::Update);
  connect(window, &MappingWindow::Save, this, &MappingWidget::SaveSettings);
  connect(window, &MappingWindow::ConfigChanged, this, &MappingWidget::ConfigChanged);
}

MappingWindow* MappingWidget::GetParent() const
{
  return m_window;
}

ControllerEmu::EmulatedController* MappingWidget::GetController() const
{
  return m_window->GetController();
}

int MappingWidget::GetPort() const
{
  return m_window->GetPort();
}

QGroupBox* MappingWidget::CreateGroupBox(ControllerEmu::ControlGroup* group)
{
  return CreateGroupBox(tr(group->ui_name.c_str()), group);
}

MappingIndicator* MappingWidget::MakeIndicator(ControllerEmu::ControlGroup* group)
{
  switch (group->type)
  {
  case GroupType::Shake:
    return new ShakeMappingIndicator(*static_cast<ControllerEmu::Shake*>(group));
  case GroupType::MixedTriggers:
    return new MixedTriggersIndicator(*static_cast<ControllerEmu::MixedTriggers*>(group));
  case GroupType::Tilt:
    return new TiltIndicator(*static_cast<ControllerEmu::Tilt*>(group));
  case GroupType::Cursor:
    return new CursorIndicator(*static_cast<ControllerEmu::Cursor*>(group));
  case GroupType::Force:
    return new SwingIndicator(*static_cast<ControllerEmu::Force*>(group));
  case GroupType::IMUAccelerometer:
    return new AccelerometerMappingIndicator(*static_cast<ControllerEmu::IMUAccelerometer*>(group));
  case GroupType::IMUGyroscope:
    return new GyroMappingIndicator(*static_cast<ControllerEmu::IMUGyroscope*>(group));
  case GroupType::Stick:
    return new AnalogStickIndicator(*static_cast<ControllerEmu::ReshapableInput*>(group));
  case GroupType::IRPassthrough:
    return new IRPassthroughMappingIndicator(*static_cast<ControllerEmu::IRPassthrough*>(group));
  default:
    return nullptr;
  }
}

void MappingWidget::AddEnabledToggle(QFormLayout* form, ControllerEmu::ControlGroup* group)
{
  AddSettingWidget(form, group->enabled_setting.get());

  // Every row added after the "Enabled" row (including rows added or inserted later) has its
  // label greyed out while the group is disabled. The state is polled on each tick and only
  // acted on when it flips; the labels start out enabled.
  const int first_row = form->rowCount();
  bool labels_enabled = true;
  connect(this, &MappingWidget::Update, this, [form, group, first_row, labels_enabled]() mutable {
    if (group->enabled_setting->GetValue() == labels_enabled)
      return;
    labels_enabled = !labels_enabled;

    for (int row = first_row; row < form->rowCount(); ++row)
    {
      QLayoutItem* const label = form->itemAt(row, QFormLayout::LabelRole);
      if (label != nullptr && label->widget() != nullptr)
        label->widget()->setEnabled(labels_enabled);
    }
  });
}

void MappingWidget::AddMousePointingButton(QFormLayout* form, ControllerEmu::ControlGroup* group)
{
  auto* const cursor = static_cast<ControllerEmu::Cursor*>(group);
  auto* const use_mouse = new QPushButton(tr("Use Mouse Controlled Pointing"));
  form->insertRow(2, use_mouse);

  connect(use_mouse, &QPushButton::clicked, this, [this, cursor] {
    // Map the four directions to the mouse cursor axes of the default device. The device
    // prefix is dropped when it is the controller's own default device anyway.
    std::string prefix = g_controller_interface.GetDefaultDeviceString() + ":";
    if (prefix == GetController()->GetDefaultDevice().ToString() + ":")
      prefix.clear();

    static constexpr const char* axes[] = {"Cursor Y-", "Cursor Y+", "Cursor X-", "Cursor X+"};
    for (int direction = 0; direction < 4; ++direction)
      cursor->SetControlExpression(direction, fmt::format("`{}{}`", prefix, axes[direction]));
    cursor->SetRelativeInput(false);

    emit ConfigChanged();
    GetController()->UpdateReferences(g_controller_interface);
  });
}

QGroupBox* MappingWidget::CreateGroupBox(const QString& name, ControllerEmu::ControlGroup* group)
{
  auto* const box = new QGroupBox(name);
  auto* const form = new QFormLayout();
  box->setLayout(form);

  // 1. The live indicator, centred on top.
  MappingIndicator* const indicator = MakeIndicator(group);
  if (indicator != nullptr)
  {
    auto* const centred = new QBoxLayout(QBoxLayout::Direction::Down);
    centred->addWidget(indicator);
    centred->setAlignment(Qt::AlignCenter);
    form->addRow(centred);
    connect(this, &MappingWidget::Update, indicator, qOverload<>(&MappingIndicator::update));
  }

  // 2. The "Enabled" setting, if the group can be switched off.
  if (group->HasEnabledSetting())
    AddEnabledToggle(form, group);

  // 3. Calibration for sticks, cursors, tilt and swing.
  if (indicator != nullptr && HasCalibration(group->type))
  {
    form->addRow(new CalibrationWidget(*this, *static_cast<ControllerEmu::ReshapableInput*>(group),
                                       *static_cast<ReshapableInputIndicator*>(indicator)));
  }

  // 4. One mapping button per control. Groups with an indicator draw their own state, so
  //    the per-button indicators are only added when there is none.
  for (const auto& control : group->controls)
    CreateControl(control.get(), form, indicator == nullptr);

  // 5. The normal numeric settings, and a button for the advanced ones.
  AddSettingWidgets(form, group, ControllerEmu::SettingVisibility::Normal);

  const bool has_advanced = std::ranges::any_of(group->numeric_settings, [](const auto& setting) {
    return setting->GetVisibility() == ControllerEmu::SettingVisibility::Advanced;
  });
  if (has_advanced)
  {
    auto* const advanced = new QPushButton(tr("Advanced"));
    form->addRow(advanced);
    connect(advanced, &QPushButton::clicked, this,
            [this, group] { ShowAdvancedControlGroupDialog(group); });
  }

  // 6. Cursors get a shortcut that maps them to the mouse (inserted as the third row).
  if (group->type == GroupType::Cursor)
    AddMousePointingButton(form, group);

  return box;
}

void MappingWidget::AddSettingWidgets(QFormLayout* layout, ControllerEmu::ControlGroup* group,
                                      ControllerEmu::SettingVisibility visibility)
{
  for (const auto& setting : group->numeric_settings)
  {
    if (setting->GetVisibility() == visibility)
      AddSettingWidget(layout, setting.get());
  }
}

void MappingWidget::AddSettingWidget(QFormLayout* layout,
                                     ControllerEmu::NumericSettingBase* setting)
{
  QWidget* editor;
  if (setting->GetType() == ControllerEmu::SettingType::Double)
    editor = new MappingDouble(this, static_cast<ControllerEmu::NumericSetting<double>*>(setting));
  else if (setting->GetType() == ControllerEmu::SettingType::Bool)
    editor = new MappingBool(this, static_cast<ControllerEmu::NumericSetting<bool>*>(setting));
  else
    return;  // No editor for the other setting types (yet).

  // The editor stretches; the "..." expression button next to it does not.
  editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* const row = new QHBoxLayout;
  row->addWidget(editor);
  row->addWidget(CreateSettingAdvancedMappingButton(*setting));

  layout->addRow(tr(setting->GetUIName()), row);
}

void MappingWidget::ShowAdvancedControlGroupDialog(ControllerEmu::ControlGroup* group)
{
  QDialog dialog{this};
  dialog.setWindowTitle(tr(group->ui_name.c_str()));

  auto* const settings_box = new QGroupBox(tr("Advanced Settings"));
  auto* const form = new QFormLayout();
  AddSettingWidgets(form, group, ControllerEmu::SettingVisibility::Advanced);

  auto* const reset_all = new QPushButton(tr("Reset All"));
  form->addRow(reset_all);
  connect(reset_all, &QPushButton::clicked, &dialog, [this, group] {
    for (const auto& setting : group->numeric_settings)
    {
      if (setting->GetVisibility() == ControllerEmu::SettingVisibility::Advanced)
        setting->SetToDefault();
    }
    emit ConfigChanged();
  });

  auto* const close_box = new QDialogButtonBox(QDialogButtonBox::Close);
  settings_box->setLayout(form);

  auto* const dialog_layout = new QVBoxLayout();
  dialog_layout->addWidget(settings_box);
  dialog_layout->addWidget(close_box);
  dialog.setLayout(dialog_layout);

  // Start with the focus on the button box: a focused spin box of an expression-backed
  // setting stops following its live value, which would look odd right after opening.
  close_box->setFocus();

  // Let the new editors pick up the current values.
  emit ConfigChanged();

  connect(close_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  dialog.exec();
}

QGroupBox* MappingWidget::CreateControlsBox(const QString& name, ControllerEmu::ControlGroup* group,
                                            int columns)
{
  auto* const box = new QGroupBox(name);
  auto* const row = new QHBoxLayout();
  box->setLayout(row);

  std::vector<QFormLayout*> column_forms(static_cast<std::size_t>(columns));
  for (QFormLayout*& column_form : column_forms)
  {
    column_form = new QFormLayout();
    row->addLayout(column_form);
  }

  // Deal the controls out over the columns: 0, 1, ..., columns - 1, 0, 1, ...
  std::size_t index = 0;
  for (const auto& control : group->controls)
    CreateControl(control.get(), column_forms[index++ % column_forms.size()], true);

  return box;
}

void MappingWidget::CreateControl(const ControllerEmu::Control* control, QFormLayout* layout,
                                  bool indicator)
{
  ControlReference* const reference = control->control_ref.get();
  const bool is_input = reference->IsInput();

  // Modifier inputs are recognised by name.
  MappingButton::ControlType type = MappingButton::ControlType::Output;
  if (is_input)
  {
    type = control->name == "Modifier" ? MappingButton::ControlType::ModifierInput :
                                         MappingButton::ControlType::NormalInput;
  }

  auto* const button = new MappingButton(this, reference, type);

  if (is_input)
  {
    // With iterative mapping, finishing one input button moves on to the next one.
    if (m_last_input_button != nullptr)
    {
      connect(m_last_input_button, &MappingButton::QueueNextButtonMapping, this,
              [this, button] { emit m_window->QueueInputDetection(button); });
    }
    m_last_input_button = button;
  }

  button->setMinimumWidth(100);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  const QString label = control->translate == ControllerEmu::Translatability::Translate ?
                            tr(control->ui_name.c_str()) :
                            QString::fromStdString(control->ui_name);

  if (!indicator || !is_input)
  {
    layout->addRow(label, button);
    return;
  }

  // A thin level bar to the left of the button.
  auto* const level = new ButtonIndicator{reference};
  connect(this, &MappingWidget::Update, level, qOverload<>(&MappingIndicator::update));

  auto* const pair = new QHBoxLayout;
  pair->setSpacing(0);
  pair->addWidget(level);
  pair->addWidget(button);
  layout->addRow(label, pair);
}

QPushButton*
MappingWidget::CreateSettingAdvancedMappingButton(ControllerEmu::NumericSettingBase& setting)
{
  auto* const button = new QPushButton(tr("..."));
  const int text_width = QFontMetrics(font()).boundingRect(button->text()).width();
  button->setFixedWidth(text_width * 2);

  connect(button, &QPushButton::clicked, this, [this, &setting] {
    // Turn a plain value into an equivalent expression so it can be edited as one.
    if (setting.IsSimpleValue())
      setting.SetExpressionFromValue();

    // Shows the expression marker while editing, and cancels any mapping in progress.
    emit ConfigChanged();

    IOWindow editor(GetParent(), GetController(), &setting.GetInputReference(),
                    IOWindow::Type::Input);
    editor.exec();

    // Back to a plain value if the expression ended up being a constant.
    setting.SimplifyIfPossible();

    emit ConfigChanged();
    SaveSettings();
  });

  return button;
}
