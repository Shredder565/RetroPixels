// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include <QToolBar>

class QAction;

namespace Core
{
enum class State;
}

// The main window's tool bar: game list, emulation, settings and (in debug mode) stepping
// buttons. Buttons only emit signals; MainWindow does the work.
class ToolBar final : public QToolBar
{
  Q_OBJECT

public:
  explicit ToolBar(QWidget* parent = nullptr);

  void closeEvent(QCloseEvent*) override;
signals:
  void OpenPressed();
  void RefreshPressed();
  void PlayPressed();
  void PausePressed();
  void StopPressed();
  void FullScreenPressed();
  void ScreenShotPressed();

  void SettingsPressed();
  void ControllersPressed();
  void GraphicsPressed();

  void StepPressed();
  void StepOverPressed();
  void StepOutPressed();
  void SkipPressed();
  void ShowPCPressed();
  void SetPCPressed();

private:
  void AddButtons();
  void EqualizeButtonWidths();
  void ApplyIcons();

  void ShowEmulationState(Core::State state);
  void ShowDebugButtons(bool visible);
  void EnableSteppingButtons();
  void ShowPlayOrPause(bool show_pause);

  // Debugger buttons, in tool bar order: Step, Step Over, Step Out, Skip, Show PC, Set PC.
  std::array<QAction*, 6> m_debug{};

  QAction* m_open = nullptr;
  QAction* m_refresh = nullptr;
  QAction* m_play_pause = nullptr;
  QAction* m_stop = nullptr;
  QAction* m_fullscreen = nullptr;
  QAction* m_screenshot = nullptr;
  QAction* m_settings = nullptr;
  QAction* m_graphics = nullptr;
  QAction* m_controllers = nullptr;
};
