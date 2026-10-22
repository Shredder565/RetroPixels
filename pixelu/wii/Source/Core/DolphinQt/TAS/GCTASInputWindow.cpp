// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/GCTASInputWindow.h"

#include <array>

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSpacerItem>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"

#include "DolphinQt/QtUtils/AspectRatioWidget.h"
#include "DolphinQt/TAS/TASCheckBox.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"

namespace
{
struct PadButton
{
  const char* label;
  const char* group;
  const char* control;
  int row;
  int column;
};

// In creation order; row/column give the place in the buttons grid.
constexpr std::array<PadButton, 12> PAD_BUTTONS{{
    {"&A", GCPad::BUTTONS_GROUP, GCPad::A_BUTTON, 0, 0},
    {"&B", GCPad::BUTTONS_GROUP, GCPad::B_BUTTON, 0, 1},
    {"&X", GCPad::BUTTONS_GROUP, GCPad::X_BUTTON, 0, 2},
    {"&Y", GCPad::BUTTONS_GROUP, GCPad::Y_BUTTON, 0, 3},
    {"&Z", GCPad::BUTTONS_GROUP, GCPad::Z_BUTTON, 0, 4},
    {"&START", GCPad::BUTTONS_GROUP, GCPad::START_BUTTON, 1, 0},
    {"&L", GCPad::TRIGGERS_GROUP, GCPad::L_DIGITAL, 0, 5},
    {"&R", GCPad::TRIGGERS_GROUP, GCPad::R_DIGITAL, 0, 6},
    {"L&eft", GCPad::DPAD_GROUP, DIRECTION_LEFT, 1, 1},
    {"&Up", GCPad::DPAD_GROUP, DIRECTION_UP, 1, 2},
    {"&Down", GCPad::DPAD_GROUP, DIRECTION_DOWN, 1, 3},
    {"R&ight", GCPad::DPAD_GROUP, DIRECTION_RIGHT, 1, 4},
}};

// The grid is filled row by row, left to right.
constexpr std::array<int, 12> GRID_FILL_ORDER{0, 1, 2, 3, 4, 6, 7, 5, 8, 9, 10, 11};

constexpr int STICK_MIN = 1;
constexpr int STICK_MAX = 255;
constexpr int TRIGGER_MAX = 255;
}  // namespace

GCTASInputWindow::GCTASInputWindow(QWidget* parent, int controller_id)
    : TASInputWindow(parent), m_port(controller_id)
{
  setWindowTitle(tr("GameCube TAS Input %1").arg(controller_id + 1));

  m_main_stick_box =
      CreateStickInputs(tr("Main Stick"), GCPad::MAIN_STICK_GROUP, &m_overrider, STICK_MIN,
                        STICK_MIN, STICK_MAX, STICK_MAX, Qt::Key_F, Qt::Key_G);
  m_c_stick_box = CreateStickInputs(tr("C Stick"), GCPad::C_STICK_GROUP, &m_overrider, STICK_MIN,
                                    STICK_MIN, STICK_MAX, STICK_MAX, Qt::Key_H, Qt::Key_J);

  auto* sticks_row = new QHBoxLayout;
  sticks_row->addWidget(new AspectRatioWidget(m_main_stick_box, 1, 1.02f));
  sticks_row->addWidget(new AspectRatioWidget(m_c_stick_box, 1, 1.02f));

  m_triggers_box = BuildTriggersBox();
  m_buttons_box = BuildButtonsBox();

  auto* column = new QVBoxLayout;
  column->addLayout(sticks_row);
  column->addWidget(m_triggers_box);
  column->addWidget(m_buttons_box);
  column->addWidget(m_settings_box);
  SetupScrollArea(column);

  // Open big enough to show everything without scrolling.
  const QSize wanted = m_scroll_widget->sizeHint();
  const int slack = style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 10;
  resize(wanted.width() + slack, wanted.height() + slack);
}

QGroupBox* GCTASInputWindow::BuildTriggersBox()
{
  auto* box = new QGroupBox(tr("Triggers"));

  auto* left = CreateSliderValuePairLayout(tr("Left"), GCPad::TRIGGERS_GROUP, GCPad::L_ANALOG,
                                           &m_overrider, 0, 0, 0, TRIGGER_MAX, Qt::Key_N, box);
  auto* right = CreateSliderValuePairLayout(tr("Right"), GCPad::TRIGGERS_GROUP, GCPad::R_ANALOG,
                                            &m_overrider, 0, 0, 0, TRIGGER_MAX, Qt::Key_M, box);

  auto* stack = new QVBoxLayout;
  stack->addLayout(left);
  stack->addLayout(right);
  box->setLayout(stack);
  return box;
}

QGroupBox* GCTASInputWindow::BuildButtonsBox()
{
  std::array<TASCheckBox*, PAD_BUTTONS.size()> made{};
  for (std::size_t i = 0; i < PAD_BUTTONS.size(); ++i)
  {
    const PadButton& spec = PAD_BUTTONS[i];
    made[i] = CreateButton(QString::fromLatin1(spec.label), spec.group, spec.control, &m_overrider);
  }

  auto* grid = new QGridLayout;
  for (const int index : GRID_FILL_ORDER)
  {
    const PadButton& spec = PAD_BUTTONS[static_cast<std::size_t>(index)];
    grid->addWidget(made[static_cast<std::size_t>(index)], spec.row, spec.column);
  }
  grid->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding), 0, 7);

  auto* box = new QGroupBox(tr("Buttons"));
  box->setLayout(grid);
  return box;
}

void GCTASInputWindow::hideEvent(QHideEvent*)
{
  Pad::GetConfig()->GetController(m_port)->ClearInputOverrideFunction();
}

void GCTASInputWindow::showEvent(QShowEvent*)
{
  Pad::GetConfig()->GetController(m_port)->SetInputOverrideFunction(
      m_overrider.GetInputOverrideFunction());
}
