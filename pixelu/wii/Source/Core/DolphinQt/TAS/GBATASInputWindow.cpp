// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/GBATASInputWindow.h"

#include <array>

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSpacerItem>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAPadEmu.h"

#include "DolphinQt/TAS/TASCheckBox.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"

namespace
{
struct GBAButton
{
  const char* label;
  const char* group;
  const char* control;
};

// Creation order: face and shoulder buttons, then the D-pad.
enum Slot : std::size_t
{
  BTN_B,
  BTN_A,
  BTN_L,
  BTN_R,
  BTN_SELECT,
  BTN_START,
  BTN_LEFT,
  BTN_UP,
  BTN_DOWN,
  BTN_RIGHT,
  SLOT_COUNT
};

constexpr std::array<GBAButton, SLOT_COUNT> GBA_BUTTONS{{
    {"&B", GBAPad::BUTTONS_GROUP, GBAPad::B_BUTTON},
    {"&A", GBAPad::BUTTONS_GROUP, GBAPad::A_BUTTON},
    {"&L", GBAPad::BUTTONS_GROUP, GBAPad::L_BUTTON},
    {"&R", GBAPad::BUTTONS_GROUP, GBAPad::R_BUTTON},
    {"SELE&CT", GBAPad::BUTTONS_GROUP, GBAPad::SELECT_BUTTON},
    {"&START", GBAPad::BUTTONS_GROUP, GBAPad::START_BUTTON},
    {"L&eft", GBAPad::DPAD_GROUP, DIRECTION_LEFT},
    {"&Up", GBAPad::DPAD_GROUP, DIRECTION_UP},
    {"&Down", GBAPad::DPAD_GROUP, DIRECTION_DOWN},
    {"R&ight", GBAPad::DPAD_GROUP, DIRECTION_RIGHT},
}};
}  // namespace

GBATASInputWindow::GBATASInputWindow(QWidget* parent, int controller_id)
    : TASInputWindow(parent), m_port(controller_id)
{
  setWindowTitle(tr("GBA TAS Input %1").arg(controller_id + 1));

  std::array<TASCheckBox*, SLOT_COUNT> button{};
  for (std::size_t slot = 0; slot < SLOT_COUNT; ++slot)
  {
    const GBAButton& spec = GBA_BUTTONS[slot];
    button[slot] =
        CreateButton(QString::fromLatin1(spec.label), spec.group, spec.control, &m_overrider);
  }

  // D-pad on the top row, shoulders and face buttons below, SELECT/START along the bottom.
  auto* grid = new QGridLayout;
  grid->addWidget(button[BTN_LEFT], 0, 0);
  grid->addWidget(button[BTN_UP], 0, 1);
  grid->addWidget(button[BTN_DOWN], 0, 2);
  grid->addWidget(button[BTN_RIGHT], 0, 3);
  grid->addWidget(button[BTN_L], 1, 0);
  grid->addWidget(button[BTN_R], 1, 1);
  grid->addWidget(button[BTN_B], 1, 2);
  grid->addWidget(button[BTN_A], 1, 3);
  grid->addWidget(button[BTN_SELECT], 2, 0, 1, 2);
  grid->addWidget(button[BTN_START], 2, 2, 1, 2);
  grid->addItem(new QSpacerItem(1, 1, QSizePolicy::Expanding), 0, 4);

  auto* buttons_box = new QGroupBox(tr("Buttons"));
  buttons_box->setLayout(grid);

  auto* column = new QVBoxLayout;
  column->addWidget(buttons_box);
  column->addWidget(m_settings_box);
  SetupScrollArea(column);

  const QSize wanted = m_scroll_widget->sizeHint();
  const int slack = style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 10;
  resize(wanted.width() + slack, wanted.height() + slack);
}

void GBATASInputWindow::hideEvent(QHideEvent*)
{
  Pad::GetGBAConfig()->GetController(m_port)->ClearInputOverrideFunction();
}

void GBATASInputWindow::showEvent(QShowEvent*)
{
  Pad::GetGBAConfig()->GetController(m_port)->SetInputOverrideFunction(
      m_overrider.GetInputOverrideFunction());
}
