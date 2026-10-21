// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/WiiTASInputWindow.h"

#include <array>
#include <cmath>
#include <string>

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSpacerItem>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/MathUtil.h"

#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/System.h"

#include "DolphinQt/QtUtils/AspectRatioWidget.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/TAS/IRWidget.h"
#include "DolphinQt/TAS/TASCheckBox.h"
#include "DolphinQt/TAS/TASSpinBox.h"

#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/InputConfig.h"

using WiimoteEmu::Classic;
using WiimoteEmu::MotionPlus;
using WiimoteEmu::Nunchuk;

struct WiiTASInputWindow::MotionAxis
{
  QString name;
  const char* control;
  int rest;
  Qt::Key key;
};

struct WiiTASInputWindow::MotionRange
{
  int zero;
  int min;
  int max;
  std::optional<ControlState> scale;
};

namespace
{
constexpr float STICK_BOX_ASPECT = 1.02f;

// Accelerometer values in TAS input use 10 bits (the 8-bit calibration values shifted up).
constexpr u16 ACCEL_ZERO_G = WiimoteEmu::Wiimote::ACCEL_ZERO_G << 2;
constexpr u16 ACCEL_ONE_G = WiimoteEmu::Wiimote::ACCEL_ONE_G << 2;
constexpr u16 ACCEL_MIN = 0;
constexpr u16 ACCEL_MAX = (1 << 10) - 1;
constexpr double ACCEL_SCALE = (ACCEL_ONE_G - ACCEL_ZERO_G) / MathUtil::GRAVITY_ACCELERATION;

// MotionPlus reports with either a fine (slow) or a coarse (fast) scale. TAS input offers the fine
// step over the coarse range, so every reportable rate can be picked (plus a few in between).
constexpr double GYRO_STRETCH = static_cast<double>(MotionPlus::CALIBRATION_FAST_SCALE_DEGREES) /
                                MotionPlus::CALIBRATION_SLOW_SCALE_DEGREES;
constexpr u32 GYRO_MIN = 0;
constexpr auto GYRO_MAX = static_cast<u32>(MotionPlus::MAX_VALUE * GYRO_STRETCH);
constexpr auto GYRO_ZERO = static_cast<u32>(MotionPlus::ZERO_VALUE * GYRO_STRETCH);
constexpr double GYRO_SCALE = static_cast<float>(GYRO_MAX / 2) / MotionPlus::FAST_MAX_RAD_PER_SEC;

struct ButtonSpec
{
  const char* label;
  const char* group;
  const char* control;
};

constexpr std::array<ButtonSpec, 11> REMOTE_BUTTONS{{
    {"&A", WiimoteEmu::Wiimote::BUTTONS_GROUP, WiimoteEmu::Wiimote::A_BUTTON},
    {"&B", WiimoteEmu::Wiimote::BUTTONS_GROUP, WiimoteEmu::Wiimote::B_BUTTON},
    {"&1", WiimoteEmu::Wiimote::BUTTONS_GROUP, WiimoteEmu::Wiimote::ONE_BUTTON},
    {"&2", WiimoteEmu::Wiimote::BUTTONS_GROUP, WiimoteEmu::Wiimote::TWO_BUTTON},
    {"&+", WiimoteEmu::Wiimote::BUTTONS_GROUP, WiimoteEmu::Wiimote::PLUS_BUTTON},
    {"&-", WiimoteEmu::Wiimote::BUTTONS_GROUP, WiimoteEmu::Wiimote::MINUS_BUTTON},
    {"&HOME", WiimoteEmu::Wiimote::BUTTONS_GROUP, WiimoteEmu::Wiimote::HOME_BUTTON},
    {"&Left", WiimoteEmu::Wiimote::DPAD_GROUP, DIRECTION_LEFT},
    {"&Up", WiimoteEmu::Wiimote::DPAD_GROUP, DIRECTION_UP},
    {"&Down", WiimoteEmu::Wiimote::DPAD_GROUP, DIRECTION_DOWN},
    {"&Right", WiimoteEmu::Wiimote::DPAD_GROUP, DIRECTION_RIGHT},
}};

constexpr std::array<ButtonSpec, 2> NUNCHUK_BUTTONS{{
    {"&C", Nunchuk::BUTTONS_GROUP, Nunchuk::C_BUTTON},
    {"&Z", Nunchuk::BUTTONS_GROUP, Nunchuk::Z_BUTTON},
}};

constexpr std::array<ButtonSpec, 15> CLASSIC_BUTTONS{{
    {"&A", Classic::BUTTONS_GROUP, Classic::A_BUTTON},
    {"&B", Classic::BUTTONS_GROUP, Classic::B_BUTTON},
    {"&X", Classic::BUTTONS_GROUP, Classic::X_BUTTON},
    {"&Y", Classic::BUTTONS_GROUP, Classic::Y_BUTTON},
    {"&ZL", Classic::BUTTONS_GROUP, Classic::ZL_BUTTON},
    {"ZR", Classic::BUTTONS_GROUP, Classic::ZR_BUTTON},
    {"&+", Classic::BUTTONS_GROUP, Classic::PLUS_BUTTON},
    {"&-", Classic::BUTTONS_GROUP, Classic::MINUS_BUTTON},
    {"&HOME", Classic::BUTTONS_GROUP, Classic::HOME_BUTTON},
    {"&L", Classic::TRIGGERS_GROUP, Classic::L_DIGITAL},
    {"&R", Classic::TRIGGERS_GROUP, Classic::R_DIGITAL},
    {"L&eft", Classic::DPAD_GROUP, DIRECTION_LEFT},
    {"&Up", Classic::DPAD_GROUP, DIRECTION_UP},
    {"&Down", Classic::DPAD_GROUP, DIRECTION_DOWN},
    {"R&ight", Classic::DPAD_GROUP, DIRECTION_RIGHT},
}};

// Classic Controller grid: indexes into CLASSIC_BUTTONS, row 0 then row 1, left to right.
constexpr std::array<std::size_t, 8> CLASSIC_TOP_ROW{0, 1, 2, 3, 9, 10, 4, 5};
constexpr std::array<std::size_t, 7> CLASSIC_BOTTOM_ROW{6, 7, 8, 11, 12, 13, 14};

WiimoteEmu::ExtensionNumber ExtensionFromIniName(const std::string& name)
{
  if (name == "Nunchuk")
    return WiimoteEmu::ExtensionNumber::NUNCHUK;
  if (name == "Classic")
    return WiimoteEmu::ExtensionNumber::CLASSIC;
  return WiimoteEmu::ExtensionNumber::NONE;
}
}  // namespace

WiiTASInputWindow::WiiTASInputWindow(QWidget* parent, int num) : TASInputWindow(parent), m_num(num)
{
  m_ir_box = BuildIRBox();

  m_nunchuk_stick_box = new AspectRatioWidget(
      CreateStickInputs(tr("Nunchuk Stick"), Nunchuk::STICK_GROUP, &m_nunchuk_overrider, 0, 0, 255,
                        255, Qt::Key_F, Qt::Key_G),
      1, STICK_BOX_ASPECT);
  m_classic_left_stick_box = new AspectRatioWidget(
      CreateStickInputs(tr("Left Stick"), Classic::LEFT_STICK_GROUP, &m_classic_overrider, 0, 0, 63,
                        63, Qt::Key_F, Qt::Key_G),
      1, STICK_BOX_ASPECT);
  m_classic_right_stick_box = new AspectRatioWidget(
      CreateStickInputs(tr("Right Stick"), Classic::RIGHT_STICK_GROUP, &m_classic_overrider, 0, 0,
                        31, 31, Qt::Key_Q, Qt::Key_W),
      1, STICK_BOX_ASPECT);

  auto* pointers_row = new QHBoxLayout;
  pointers_row->addWidget(m_ir_box);
  pointers_row->addWidget(m_nunchuk_stick_box);
  pointers_row->addWidget(m_classic_left_stick_box);
  pointers_row->addWidget(m_classic_right_stick_box);

  using RI = ControllerEmu::ReshapableInput;

  const MotionRange accel_with_scale{ACCEL_ZERO_G, ACCEL_MIN, ACCEL_MAX, ACCEL_SCALE};
  // i18n: Refers to a 3D axis (used when mapping motion controls)
  const QString x_axis = tr("X");
  // i18n: Refers to a 3D axis (used when mapping motion controls)
  const QString y_axis = tr("Y");
  // i18n: Refers to a 3D axis (used when mapping motion controls)
  const QString z_axis = tr("Z");

  m_remote_accelerometer_box =
      BuildMotionBox(tr("Wii Remote Accelerometer"), WiimoteEmu::Wiimote::ACCELEROMETER_GROUP,
                     &m_wiimote_overrider, accel_with_scale,
                     {{x_axis, RI::X_INPUT_OVERRIDE, ACCEL_ZERO_G, Qt::Key_Q},
                      {y_axis, RI::Y_INPUT_OVERRIDE, ACCEL_ZERO_G, Qt::Key_W},
                      {z_axis, RI::Z_INPUT_OVERRIDE, ACCEL_ONE_G, Qt::Key_E}});

  const MotionRange gyro{static_cast<int>(GYRO_ZERO), static_cast<int>(GYRO_MIN),
                         static_cast<int>(GYRO_MAX), GYRO_SCALE};
  m_remote_gyroscope_box = BuildMotionBox(
      tr("Wii Remote Gyroscope"), WiimoteEmu::Wiimote::GYROSCOPE_GROUP, &m_wiimote_overrider, gyro,
      {{x_axis, RI::X_INPUT_OVERRIDE, static_cast<int>(GYRO_ZERO), Qt::Key_R},
       {y_axis, RI::Y_INPUT_OVERRIDE, static_cast<int>(GYRO_ZERO), Qt::Key_T},
       {z_axis, RI::Z_INPUT_OVERRIDE, static_cast<int>(GYRO_ZERO), Qt::Key_Y}});

  // The Nunchuk accelerometer uses the same 10-bit range as the remote's, without a scale.
  const MotionRange accel_plain{ACCEL_ZERO_G, ACCEL_MIN, ACCEL_MAX, std::nullopt};
  m_nunchuk_accelerometer_box = BuildMotionBox(
      tr("Nunchuk Accelerometer"), Nunchuk::ACCELEROMETER_GROUP, &m_nunchuk_overrider, accel_plain,
      {{x_axis, RI::X_INPUT_OVERRIDE, ACCEL_ZERO_G, Qt::Key_I},
       {y_axis, RI::Y_INPUT_OVERRIDE, ACCEL_ZERO_G, Qt::Key_O},
       {z_axis, RI::Z_INPUT_OVERRIDE, ACCEL_ONE_G, Qt::Key_P}});

  m_triggers_box = BuildClassicTriggersBox();
  BuildRemoteAndNunchukButtons();
  m_classic_buttons_box = BuildClassicButtonsBox();

  auto* column = new QVBoxLayout;
  column->addLayout(pointers_row);
  for (QWidget* section :
       {static_cast<QWidget*>(m_remote_accelerometer_box),
        static_cast<QWidget*>(m_remote_gyroscope_box),
        static_cast<QWidget*>(m_nunchuk_accelerometer_box), static_cast<QWidget*>(m_triggers_box),
        static_cast<QWidget*>(m_remote_buttons_box), static_cast<QWidget*>(m_nunchuk_buttons_box),
        static_cast<QWidget*>(m_classic_buttons_box), static_cast<QWidget*>(m_settings_box)})
  {
    column->addWidget(section);
  }

  SetupScrollArea(column);
}

AspectRatioWidget* WiiTASInputWindow::BuildIRBox()
{
  const QKeySequence x_keys(Qt::ALT | Qt::Key_X);
  const QKeySequence y_keys(Qt::ALT | Qt::Key_C);

  auto* group = new QGroupBox(QStringLiteral("%1 (%2/%3)")
                                  .arg(tr("IR"), x_keys.toString(QKeySequence::NativeText),
                                       y_keys.toString(QKeySequence::NativeText)));

  const int centre_x = static_cast<int>(std::round(IRWidget::IR_MAX_X / 2.));
  const int centre_y = static_cast<int>(std::round(IRWidget::IR_MAX_Y / 2.));

  auto* grid = new QGridLayout;
  TASSpinBox* x_spin = CreateSliderValuePair(
      WiimoteEmu::Wiimote::IR_GROUP, ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE,
      &m_wiimote_overrider, grid, centre_x, centre_x, IRWidget::IR_MIN_X, IRWidget::IR_MAX_X,
      x_keys, Qt::Horizontal, group);
  TASSpinBox* y_spin = CreateSliderValuePair(
      WiimoteEmu::Wiimote::IR_GROUP, ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE,
      &m_wiimote_overrider, grid, centre_y, centre_y, IRWidget::IR_MIN_Y, IRWidget::IR_MAX_Y,
      y_keys, Qt::Vertical, group);
  y_spin->setMaximumWidth(60);

  auto* camera = new IRWidget(this);
  camera->SetX(static_cast<u16>(centre_x));
  camera->SetY(static_cast<u16>(centre_y));

  connect(x_spin, &QSpinBox::valueChanged, camera, &IRWidget::SetX);
  connect(y_spin, &QSpinBox::valueChanged, camera, &IRWidget::SetY);
  connect(camera, &IRWidget::ChangedX, x_spin, &QSpinBox::setValue);
  connect(camera, &IRWidget::ChangedY, y_spin, &QSpinBox::setValue);

  auto* camera_frame = new AspectRatioWidget(camera, IRWidget::IR_MAX_X, IRWidget::IR_MAX_Y);

  // An empty cell below the view takes up spare height instead of the view itself.
  grid->addItem(new QSpacerItem(0, 0), 2, 1);
  grid->addWidget(camera_frame, 1, 1);
  group->setLayout(grid);

  return new AspectRatioWidget(group, 1, STICK_BOX_ASPECT);
}

QGroupBox* WiiTASInputWindow::BuildMotionBox(const QString& title, const char* group,
                                             InputOverrider* overrider, const MotionRange& range,
                                             const MotionAxis (&axes)[3])
{
  auto* box = new QGroupBox(title);

  std::array<QGridLayout*, 3> rows{};
  for (std::size_t i = 0; i < rows.size(); ++i)
  {
    const MotionAxis& axis = axes[i];
    rows[i] =
        CreateSliderValuePairLayout(axis.name, group, axis.control, overrider, range.zero,
                                    axis.rest, range.min, range.max, axis.key, box, range.scale);
  }

  auto* stack = new QVBoxLayout;
  for (QGridLayout* row : rows)
    stack->addLayout(row);
  box->setLayout(stack);
  return box;
}

QGroupBox* WiiTASInputWindow::BuildClassicTriggersBox()
{
  constexpr int TRIGGER_MAX = 31;
  auto* box = new QGroupBox(tr("Triggers"));

  auto* left =
      CreateSliderValuePairLayout(tr("Left"), Classic::TRIGGERS_GROUP, Classic::L_ANALOG,
                                  &m_classic_overrider, 0, 0, 0, TRIGGER_MAX, Qt::Key_N, box);
  auto* right =
      CreateSliderValuePairLayout(tr("Right"), Classic::TRIGGERS_GROUP, Classic::R_ANALOG,
                                  &m_classic_overrider, 0, 0, 0, TRIGGER_MAX, Qt::Key_M, box);

  auto* stack = new QVBoxLayout;
  stack->addLayout(left);
  stack->addLayout(right);
  box->setLayout(stack);
  return box;
}

void WiiTASInputWindow::BuildRemoteAndNunchukButtons()
{
  std::array<TASCheckBox*, REMOTE_BUTTONS.size()> remote{};
  for (std::size_t i = 0; i < remote.size(); ++i)
  {
    const ButtonSpec& spec = REMOTE_BUTTONS[i];
    remote[i] = CreateButton(QString::fromLatin1(spec.label), spec.group, spec.control,
                             &m_wiimote_overrider);
  }

  std::array<TASCheckBox*, NUNCHUK_BUTTONS.size()> nunchuk{};
  for (std::size_t i = 0; i < nunchuk.size(); ++i)
  {
    const ButtonSpec& spec = NUNCHUK_BUTTONS[i];
    nunchuk[i] = CreateButton(QString::fromLatin1(spec.label), spec.group, spec.control,
                              &m_nunchuk_overrider);
  }

  // Six buttons on the first row, HOME and the D-pad on the second.
  auto* remote_grid = new QGridLayout;
  for (std::size_t i = 0; i < remote.size(); ++i)
  {
    const bool first_row = i < 6;
    const int column = static_cast<int>(first_row ? i : i - 6);
    remote_grid->addWidget(remote[i], first_row ? 0 : 1, column);
  }
  remote_grid->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding), 0, 7);

  m_remote_buttons_box = new QGroupBox(tr("Wii Remote Buttons"));
  m_remote_buttons_box->setLayout(remote_grid);

  auto* nunchuk_row = new QHBoxLayout;
  for (TASCheckBox* button : nunchuk)
    nunchuk_row->addWidget(button);
  nunchuk_row->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding));

  m_nunchuk_buttons_box = new QGroupBox(tr("Nunchuk Buttons"));
  m_nunchuk_buttons_box->setLayout(nunchuk_row);
}

QGroupBox* WiiTASInputWindow::BuildClassicButtonsBox()
{
  std::array<TASCheckBox*, CLASSIC_BUTTONS.size()> made{};
  for (std::size_t i = 0; i < made.size(); ++i)
  {
    const ButtonSpec& spec = CLASSIC_BUTTONS[i];
    made[i] = CreateButton(QString::fromLatin1(spec.label), spec.group, spec.control,
                           &m_classic_overrider);
  }

  auto* grid = new QGridLayout;
  for (std::size_t col = 0; col < CLASSIC_TOP_ROW.size(); ++col)
    grid->addWidget(made[CLASSIC_TOP_ROW[col]], 0, static_cast<int>(col));
  for (std::size_t col = 0; col < CLASSIC_BOTTOM_ROW.size(); ++col)
    grid->addWidget(made[CLASSIC_BOTTOM_ROW[col]], 1, static_cast<int>(col));
  grid->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding), 0, 8);

  auto* box = new QGroupBox(tr("Classic Buttons"));
  box->setLayout(grid);
  return box;
}

WiimoteEmu::Wiimote* WiiTASInputWindow::GetWiimote()
{
  return static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(m_num));
}

ControllerEmu::Attachments* WiiTASInputWindow::GetAttachments()
{
  return static_cast<ControllerEmu::Attachments*>(
      GetWiimote()->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Attachments));
}

WiimoteEmu::Extension* WiiTASInputWindow::GetExtension()
{
  const auto& attachments = GetAttachments()->GetAttachmentList();
  return static_cast<WiimoteEmu::Extension*>(attachments[m_active_extension].get());
}

void WiiTASInputWindow::UpdateExtension(const int extension)
{
  const auto chosen = static_cast<WiimoteEmu::ExtensionNumber>(extension);
  if (chosen == m_active_extension)
    return;

  m_active_extension = chosen;
  UpdateControlVisibility();
  UpdateInputOverrideFunction();
}

void WiiTASInputWindow::UpdateMotionPlus(const bool attached)
{
  if (m_is_motion_plus_attached == attached)
    return;

  m_is_motion_plus_attached = attached;
  UpdateControlVisibility();
}

void WiiTASInputWindow::LoadExtensionAndMotionPlus()
{
  WiimoteEmu::Wiimote* const remote = GetWiimote();

  if (Core::IsRunning(Core::System::GetInstance()))
  {
    m_active_extension = remote->GetActiveExtensionNumber();
    m_is_motion_plus_attached = remote->GetMotionPlusSetting().GetValue();
  }
  else
  {
    // No emulation: take the attachments from the saved controller profile.
    Common::IniFile profile;
    profile.Load(File::GetUserPath(D_CONFIG_IDX) + "WiimoteNew.ini");
    const std::string section = fmt::format("Wiimote{}", m_num + 1);

    std::string extension_name;
    profile.GetIfExists(section, "Extension", &extension_name);
    m_active_extension = ExtensionFromIniName(extension_name);

    m_is_motion_plus_attached = true;
    profile.GetIfExists(section, "Extension/Attach MotionPlus", &m_is_motion_plus_attached);
  }

  UpdateControlVisibility();
  UpdateInputOverrideFunction();

  // Follow attachment changes made while the window is open (they arrive on the CPU thread).
  m_motion_plus_callback_id = remote->GetMotionPlusSetting().AddCallback([this](bool attached) {
    QueueOnObject(this, [this, attached] { UpdateMotionPlus(attached); });
  });
  m_attachment_callback_id =
      GetAttachments()->GetAttachmentSetting().AddCallback([this](int extension_index) {
        QueueOnObject(this, [this, extension_index] { UpdateExtension(extension_index); });
      });
}

void WiiTASInputWindow::UpdateControlVisibility()
{
  const bool nunchuk = m_active_extension == WiimoteEmu::ExtensionNumber::NUNCHUK;
  const bool classic = m_active_extension == WiimoteEmu::ExtensionNumber::CLASSIC;

  if (nunchuk)
    setWindowTitle(tr("Wii TAS Input %1 - Wii Remote + Nunchuk").arg(m_num + 1));
  else if (classic)
    setWindowTitle(tr("Wii TAS Input %1 - Classic Controller").arg(m_num + 1));
  else
    setWindowTitle(tr("Wii TAS Input %1 - Wii Remote").arg(m_num + 1));

  // The Classic Controller replaces the remote's own inputs; the Nunchuk adds to them.
  m_ir_box->setVisible(!classic);
  m_nunchuk_stick_box->setVisible(nunchuk);
  m_classic_right_stick_box->setVisible(classic);
  m_classic_left_stick_box->setVisible(classic);
  m_remote_accelerometer_box->setVisible(!classic);
  m_remote_gyroscope_box->setVisible(!classic && m_is_motion_plus_attached);
  m_nunchuk_accelerometer_box->setVisible(nunchuk);
  m_triggers_box->setVisible(classic);
  m_remote_buttons_box->setVisible(!classic);
  m_nunchuk_buttons_box->setVisible(nunchuk);
  m_classic_buttons_box->setVisible(classic);

  // Re-run the layout first, or the stick and IR views can end up padded with empty space after
  // switching attachments.
  m_scroll_widget->layout()->activate();
  const QSize wanted = m_scroll_widget->sizeHint();
  const int slack = style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 10;
  resize(wanted.width() + slack, wanted.height() + slack);
}

void WiiTASInputWindow::hideEvent(QHideEvent* const event)
{
  WiimoteEmu::Wiimote* const remote = GetWiimote();
  remote->ClearInputOverrideFunction();
  remote->GetMotionPlusSetting().RemoveCallback(m_motion_plus_callback_id);

  GetExtension()->ClearInputOverrideFunction();
  GetAttachments()->GetAttachmentSetting().RemoveCallback(m_attachment_callback_id);

  TASInputWindow::hideEvent(event);
}

void WiiTASInputWindow::showEvent(QShowEvent* const event)
{
  LoadExtensionAndMotionPlus();
  TASInputWindow::showEvent(event);
}

void WiiTASInputWindow::UpdateInputOverrideFunction()
{
  switch (m_active_extension)
  {
  case WiimoteEmu::ExtensionNumber::CLASSIC:
    GetExtension()->SetInputOverrideFunction(m_classic_overrider.GetInputOverrideFunction());
    break;
  case WiimoteEmu::ExtensionNumber::NUNCHUK:
    GetWiimote()->SetInputOverrideFunction(m_wiimote_overrider.GetInputOverrideFunction());
    GetExtension()->SetInputOverrideFunction(m_nunchuk_overrider.GetInputOverrideFunction());
    break;
  default:
    GetWiimote()->SetInputOverrideFunction(m_wiimote_overrider.GetInputOverrideFunction());
    break;
  }
}
