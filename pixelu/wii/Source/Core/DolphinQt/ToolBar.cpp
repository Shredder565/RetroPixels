// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/ToolBar.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <vector>

#include <QAction>
#include <QIcon>

#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
enum DebugButton
{
  STEP,
  STEP_OVER,
  STEP_OUT,
  SKIP,
  SHOW_PC,
  SET_PC,
};

Core::State CurrentState()
{
  return Core::GetState(Core::System::GetInstance());
}
}  // namespace

ToolBar::ToolBar(QWidget* parent) : QToolBar(parent)
{
  auto& settings = Settings::Instance();

  setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  setMovable(!settings.AreWidgetsLocked());
  setFloatable(false);
  setIconSize(QSize(32, 32));
  setVisible(settings.IsToolBarVisible());

  setWindowTitle(tr("Toolbar"));
  setObjectName(QStringLiteral("toolbar"));

  AddButtons();

  connect(&settings, &Settings::ThemeChanged, this, &ToolBar::ApplyIcons);
  ApplyIcons();

  connect(&settings, &Settings::EmulationStateChanged, this,
          [this](Core::State state) { ShowEmulationState(state); });
  // The debugger refreshes after a step or a break, which is also when stepping becomes possible.
  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this,
          [this] { ShowEmulationState(CurrentState()); });
  connect(&settings, &Settings::DebugModeToggled, this, &ToolBar::ShowDebugButtons);

  // Visibility is shared with the View menu's "Show Toolbar".
  connect(&settings, &Settings::ToolBarVisibilityChanged, this, &ToolBar::setVisible);
  connect(this, &ToolBar::visibilityChanged, &settings, &Settings::SetToolBarVisible);

  connect(&settings, &Settings::WidgetLockChanged, this,
          [this](bool locked) { setMovable(!locked); });

  // Refreshing is not offered again until the requested refresh has actually begun.
  connect(&settings, &Settings::GameListRefreshRequested, this,
          [this] { m_refresh->setEnabled(false); });
  connect(&settings, &Settings::GameListRefreshStarted, this,
          [this] { m_refresh->setEnabled(true); });

  ShowEmulationState(CurrentState());
  ShowDebugButtons(settings.IsDebugModeEnabled());
}

void ToolBar::AddButtons()
{
  // i18n: "Step" is a verb: run the emulated code one instruction at a time.
  m_debug[STEP] = addAction(tr("Step"), this, &ToolBar::StepPressed);
  // i18n: "Step" is a verb: run one instruction, but run a called function to its end.
  m_debug[STEP_OVER] = addAction(tr("Step Over"), this, &ToolBar::StepOverPressed);
  // i18n: "Step" is a verb: run until the current function returns.
  m_debug[STEP_OUT] = addAction(tr("Step Out"), this, &ToolBar::StepOutPressed);
  m_debug[SKIP] = addAction(tr("Skip"), this, &ToolBar::SkipPressed);
  // i18n: PC means program counter (the address of the next instruction).
  m_debug[SHOW_PC] = addAction(tr("Show PC"), this, &ToolBar::ShowPCPressed);
  // i18n: PC means program counter (the address of the next instruction).
  m_debug[SET_PC] = addAction(tr("Set PC"), this, &ToolBar::SetPCPressed);

  m_open = addAction(tr("Open"), this, &ToolBar::OpenPressed);
  m_refresh = addAction(tr("Refresh"), this, &ToolBar::RefreshPressed);
  // Enabled once the game list starts its first scan.
  m_refresh->setEnabled(false);

  addSeparator();

  // Play and Pause share one button; ShowPlayOrPause swaps text, icon and signal.
  m_play_pause = addAction(tr("Play"), this, &ToolBar::PlayPressed);
  m_stop = addAction(tr("Stop"), this, &ToolBar::StopPressed);
  m_fullscreen = addAction(tr("FullScr"), this, &ToolBar::FullScreenPressed);
  m_screenshot = addAction(tr("ScrShot"), this, &ToolBar::ScreenShotPressed);

  addSeparator();

  m_settings = addAction(tr("Config"), this, &ToolBar::SettingsPressed);
  m_graphics = addAction(tr("Graphics"), this, &ToolBar::GraphicsPressed);
  m_controllers = addAction(tr("Controllers"), this, &ToolBar::ControllersPressed);

  EqualizeButtonWidths();
}

void ToolBar::EqualizeButtonWidths()
{
  // Give the buttons (all but Refresh) a common minimum width of 85% of the widest one, measured
  // from their text alone, so the bar looks even without being too wide.
  std::vector<QWidget*> buttons;
  for (QAction* action : {m_open, m_play_pause, m_stop, m_fullscreen, m_screenshot, m_settings,
                          m_graphics, m_controllers})
  {
    buttons.push_back(widgetForAction(action));
  }
  for (QAction* action : m_debug)
    buttons.push_back(widgetForAction(action));

  int widest = 0;
  for (const QWidget* button : buttons)
    widest = std::max(widest, button->sizeHint().width());

  const int minimum = static_cast<int>(widest * 0.85);
  for (QWidget* button : buttons)
    button->setMinimumWidth(minimum);
}

void ToolBar::ApplyIcons()
{
  static constexpr const char* DEBUG_ICONS[] = {"debugger_step_in",  "debugger_step_over",
                                                "debugger_step_out", "debugger_skip",
                                                "debugger_show_pc",  "debugger_set_pc"};
  for (std::size_t i = 0; i < m_debug.size(); ++i)
    m_debug[i]->setIcon(Resources::GetThemeIcon(DEBUG_ICONS[i]));

  m_open->setIcon(Resources::GetThemeIcon("open"));
  m_refresh->setIcon(Resources::GetThemeIcon("refresh"));

  const Core::State state = CurrentState();
  const bool running_unpaused = state != Core::State::Uninitialized && state != Core::State::Paused;
  m_play_pause->setIcon(Resources::GetThemeIcon(running_unpaused ? "pause" : "play"));

  m_stop->setIcon(Resources::GetThemeIcon("stop"));
  m_fullscreen->setIcon(Resources::GetThemeIcon("fullscreen"));
  m_screenshot->setIcon(Resources::GetThemeIcon("screenshot"));
  m_settings->setIcon(Resources::GetThemeIcon("config"));
  m_controllers->setIcon(Resources::GetThemeIcon("classic"));
  m_graphics->setIcon(Resources::GetThemeIcon("graphics"));
}

void ToolBar::ShowEmulationState(Core::State state)
{
  const bool has_game = state != Core::State::Uninitialized;
  for (QAction* action : {m_stop, m_fullscreen, m_screenshot})
    action->setEnabled(has_game);

  ShowPlayOrPause(has_game && state != Core::State::Paused);
  EnableSteppingButtons();
}

void ToolBar::ShowDebugButtons(bool visible)
{
  for (QAction* action : m_debug)
    action->setVisible(visible);
  EnableSteppingButtons();
}

void ToolBar::EnableSteppingButtons()
{
  // Stepping needs a paused core. Show PC works at any time.
  const bool paused = CurrentState() == Core::State::Paused;
  for (const int button : {STEP, STEP_OVER, STEP_OUT, SKIP, SET_PC})
    m_debug[static_cast<std::size_t>(button)]->setEnabled(paused);
}

void ToolBar::ShowPlayOrPause(bool show_pause)
{
  // Drop whatever the button was wired to before rewiring it.
  disconnect(m_play_pause, nullptr, nullptr, nullptr);

  m_play_pause->setText(show_pause ? tr("Pause") : tr("Play"));
  m_play_pause->setIcon(Resources::GetThemeIcon(show_pause ? "pause" : "play"));
  connect(m_play_pause, &QAction::triggered, this,
          show_pause ? &ToolBar::PausePressed : &ToolBar::PlayPressed);
}

void ToolBar::closeEvent(QCloseEvent*)
{
  // Closing the floating bar hides it, like unchecking "Show Toolbar".
  Settings::Instance().SetToolBarVisible(false);
}
