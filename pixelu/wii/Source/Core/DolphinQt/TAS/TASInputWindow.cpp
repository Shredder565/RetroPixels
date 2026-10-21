// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/TASInputWindow.h"

#include <cmath>
#include <utility>

#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/AspectRatioWidget.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/TAS/StickWidget.h"
#include "DolphinQt/TAS/TASCheckBox.h"
#include "DolphinQt/TAS/TASSlider.h"
#include "DolphinQt/TAS/TASSpinBox.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/StickGate.h"

namespace
{
QKeySequence AltShortcut(Qt::Key key)
{
  return QKeySequence(Qt::ALT | key);
}

QString ShortcutText(const QKeySequence& sequence)
{
  return sequence.toString(QKeySequence::NativeText);
}

int HalfWay(int max)
{
  return static_cast<int>(std::round(max / 2.));
}
}  // namespace

void InputOverrider::AddFunction(std::string_view group_name, std::string_view control_name,
                                 OverrideFunction function)
{
  m_by_control.emplace(ControlKey{group_name, control_name}, std::move(function));
}

ControllerEmu::InputOverrideFunction InputOverrider::GetInputOverrideFunction() const
{
  return [this](std::string_view group_name, std::string_view control_name,
                ControlState controller_state) -> std::optional<ControlState> {
    const auto found = m_by_control.find(ControlKey{group_name, control_name});
    if (found == m_by_control.end())
      return std::nullopt;
    return found->second(controller_state);
  };
}

TASInputWindow::TASInputWindow(QWidget* parent) : QDialog(parent)
{
  setWindowIcon(Resources::GetAppIcon());

  auto* grid = new QGridLayout;

  m_use_controller = new QCheckBox(tr("Enable Controller Inpu&t"));
  m_use_controller->setToolTip(tr("Warning: Analog inputs may reset to controller values at "
                                  "random. In some cases this can be fixed by adding a deadzone."));
  grid->addWidget(m_use_controller, 0, 0, 1, 2);

  // One "label: frames" row per turbo phase; at least one frame each.
  const auto add_turbo_row = [grid](int row, const QString& caption) {
    auto* caption_label = new QLabel(caption);
    auto* frames = new QSpinBox();
    frames->setMinimum(1);
    grid->addWidget(caption_label, row, 0);
    grid->addWidget(frames, row, 1);
    return frames;
  };
  m_turbo_press_frames = add_turbo_row(1, tr("Duration of Turbo Button Press (frames):"));
  m_turbo_release_frames = add_turbo_row(2, tr("Duration of Turbo Button Release (frames):"));

  m_settings_box = new QGroupBox(tr("Settings"));
  m_settings_box->setLayout(grid);
}

int TASInputWindow::GetTurboPressFrames() const
{
  return m_turbo_press_frames->value();
}

int TASInputWindow::GetTurboReleaseFrames() const
{
  return m_turbo_release_frames->value();
}

TASCheckBox* TASInputWindow::CreateButton(const QString& text, std::string_view group_name,
                                          std::string_view control_name, InputOverrider* overrider)
{
  auto* button = new TASCheckBox(text, this);
  overrider->AddFunction(group_name, control_name, [this, button](ControlState real_value) {
    return PollButton(button, real_value);
  });
  return button;
}

QGroupBox* TASInputWindow::CreateStickInputs(const QString& text, std::string_view group_name,
                                             InputOverrider* overrider, int min_x, int min_y,
                                             int max_x, int max_y, Qt::Key x_shortcut_key,
                                             Qt::Key y_shortcut_key)
{
  const QKeySequence x_keys = AltShortcut(x_shortcut_key);
  const QKeySequence y_keys = AltShortcut(y_shortcut_key);

  auto* group = new QGroupBox(
      QStringLiteral("%1 (%2/%3)").arg(text, ShortcutText(x_keys), ShortcutText(y_keys)));

  const int centre_x = HalfWay(max_x);
  const int centre_y = HalfWay(max_y);

  auto* grid = new QGridLayout;
  TASSpinBox* x_spin = CreateSliderValuePair(grid, centre_x, max_x, x_keys, Qt::Horizontal, group);
  TASSpinBox* y_spin = CreateSliderValuePair(grid, centre_y, max_y, y_keys, Qt::Vertical, group);

  auto* pad = new StickWidget(this, static_cast<u16>(max_x), static_cast<u16>(max_y));
  pad->SetX(static_cast<u16>(centre_x));
  pad->SetY(static_cast<u16>(centre_y));

  // Keep the spin boxes and the drawn stick in step in both directions.
  connect(x_spin, &QSpinBox::valueChanged, pad, &StickWidget::SetX);
  connect(y_spin, &QSpinBox::valueChanged, pad, &StickWidget::SetY);
  connect(pad, &StickWidget::ChangedX, x_spin, &QSpinBox::setValue);
  connect(pad, &StickWidget::ChangedY, y_spin, &QSpinBox::setValue);

  auto* square = new AspectRatioWidget(pad, static_cast<float>(max_x), static_cast<float>(max_y));

  // The empty cell below the stick soaks up spare height so the stick isn't stretched.
  grid->addItem(new QSpacerItem(0, 0), 2, 1);
  grid->addWidget(square, 1, 1);
  group->setLayout(grid);

  overrider->AddFunction(group_name, ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE,
                         [this, x_spin, centre_x, min_x, max_x](ControlState real_value) {
                           return PollRanged(x_spin, centre_x, min_x, max_x, real_value);
                         });
  overrider->AddFunction(group_name, ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE,
                         [this, y_spin, centre_y, min_y, max_y](ControlState real_value) {
                           return PollRanged(y_spin, centre_y, min_y, max_y, real_value);
                         });

  return group;
}

QGridLayout* TASInputWindow::CreateSliderValuePairLayout(
    const QString& text, std::string_view group_name, std::string_view control_name,
    InputOverrider* overrider, int zero, int default_, int min, int max, Qt::Key shortcut_key,
    QWidget* shortcut_widget, std::optional<ControlState> scale)
{
  const QKeySequence keys = AltShortcut(shortcut_key);

  auto* caption = new QLabel(QStringLiteral("%1 (%2)").arg(text, ShortcutText(keys)));
  auto* grid = new QGridLayout;
  grid->addWidget(caption, 0, 0);

  CreateSliderValuePair(group_name, control_name, overrider, grid, zero, default_, min, max, keys,
                        Qt::Horizontal, shortcut_widget, scale);
  return grid;
}

TASSpinBox* TASInputWindow::CreateSliderValuePair(
    std::string_view group_name, std::string_view control_name, InputOverrider* overrider,
    QGridLayout* layout, int zero, int default_, int min, int max,
    const QKeySequence& shortcut_key_sequence, Qt::Orientation orientation,
    QWidget* shortcut_widget, std::optional<ControlState> scale)
{
  TASSpinBox* spin = CreateSliderValuePair(layout, default_, max, shortcut_key_sequence,
                                           orientation, shortcut_widget);

  // A scale means "value = zero + state * scale" (motion data); otherwise the spin box's range
  // maps linearly onto -1 .. +1 around zero.
  InputOverrider::OverrideFunction poll =
      scale.has_value() ?
          InputOverrider::OverrideFunction(
              [this, spin, zero, factor = *scale](ControlState real_value) {
                return PollScaled(spin, zero, real_value, factor);
              }) :
          InputOverrider::OverrideFunction([this, spin, zero, min, max](ControlState real_value) {
            return PollRanged(spin, zero, min, max, real_value);
          });

  overrider->AddFunction(group_name, control_name, std::move(poll));
  return spin;
}

// shortcut_widget is the container that gets shown and hidden with this control, so that windows
// reusing the same Alt shortcut for different (never simultaneously visible) controls don't clash.
TASSpinBox* TASInputWindow::CreateSliderValuePair(QGridLayout* layout, int default_, int max,
                                                  const QKeySequence& shortcut_key_sequence,
                                                  Qt::Orientation orientation,
                                                  QWidget* shortcut_widget)
{
  auto* spin = new TASSpinBox();
  spin->setRange(0, max);
  spin->setValue(default_);

  auto* slider = new TASSlider(default_, orientation);
  slider->setRange(0, max);
  slider->setValue(default_);
  slider->setFocusPolicy(Qt::ClickFocus);

  spin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
  connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);

  auto* jump = new QShortcut(shortcut_key_sequence, shortcut_widget);
  connect(jump, &QShortcut::activated, spin, [spin] {
    spin->setFocus();
    spin->selectAll();
  });

  const bool vertical = (orientation == Qt::Vertical);
  if (vertical)
  {
    // Vertical pairs sit in the right-hand column: slider above, value below.
    layout->addWidget(slider, 1, 2);
    layout->addWidget(spin, 2, 2);
    layout->setAlignment(slider, Qt::AlignHCenter);
    slider->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  }
  else
  {
    layout->addWidget(slider, 0, 1);
    layout->addWidget(spin, 0, 2);
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }

  return spin;
}

void TASInputWindow::SetupScrollArea(QLayout* layout)
{
  m_scroll_widget = new QWidget;
  m_scroll_widget->setLayout(layout);

  auto* scroller = new QScrollArea;
  scroller->setWidget(m_scroll_widget);
  scroller->setWidgetResizable(true);
  scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroller->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  auto* frame = new QVBoxLayout;
  frame->setContentsMargins(0, 0, 0, 0);
  frame->addWidget(scroller);
  setLayout(frame);

  layout->activate();
  m_scroll_widget->layout()->activate();
}

std::optional<ControlState> TASInputWindow::PollButton(TASCheckBox* button, ControlState real_value)
{
  if (m_use_controller->isChecked())
    button->OnControllerValueChanged(std::llround(real_value) > 0);

  return button->GetValue() ? 1.0 : 0.0;
}

std::optional<ControlState> TASInputWindow::PollRanged(TASSpinBox* value, int zero, int min,
                                                       int max, ControlState real_value)
{
  // The controller side maps -1 .. +1 onto 0 .. max (not min), as the spin box range does.
  const int from_controller = ControllerEmu::MapFloat<int>(real_value, zero, 0, max);
  if (m_use_controller->isChecked())
    value->OnControllerValueChanged(from_controller);

  return ControllerEmu::MapToFloat<ControlState, int>(value->GetValue(), zero, min, max);
}

std::optional<ControlState> TASInputWindow::PollScaled(TASSpinBox* value, int zero,
                                                       ControlState real_value, ControlState scale)
{
  const auto from_controller = static_cast<int>(std::llround(real_value * scale + zero));
  if (m_use_controller->isChecked())
    value->OnControllerValueChanged(from_controller);

  return (value->GetValue() - zero) / scale;
}

void TASInputWindow::changeEvent(QEvent* const event)
{
  if (event->type() == QEvent::ActivationChange)
  {
    // Tell the host whether any TAS window has focus. Moving between two of them reports true
    // twice, which is harmless.
    const bool tas_window_active =
        qobject_cast<TASInputWindow*>(QApplication::activeWindow()) != nullptr;
    Host::GetInstance()->SetTASInputFocus(tas_window_active);
  }
  QDialog::changeEvent(event);
}
