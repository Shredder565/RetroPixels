// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class ConfigBool;
class ConfigRadioInt;
class ConfigStringChoice;
class QLabel;
class QVBoxLayout;
class ToolTipCheckBox;
class ToolTipComboBox;

namespace Core
{
enum class State;
}

// "Interface" settings page: language, theme and style, game list options, and render window
// behaviour (including mouse cursor handling).
class InterfacePane final : public QWidget
{
  Q_OBJECT
public:
  explicit InterfacePane(QWidget* parent = nullptr);

private:
  void BuildInterfaceGroup(QVBoxLayout* page);
  void BuildRenderWindowGroup(QVBoxLayout* page);
  void FillStyleCombo();
  void WireUp();

  void RefreshDebugToggle();
  void SelectCurrentStyle();
  void OnStyleSelected();
  void OnLanguageSelected();
  void OnEmulationStateChanged(Core::State state);

  ConfigStringChoice* m_language;
  ConfigStringChoice* m_theme;
  QLabel* m_style_label;
  ToolTipComboBox* m_style;

  ConfigBool* m_builtin_titles;
  ConfigBool* m_covers;
  ToolTipCheckBox* m_debug_ui;
  ConfigBool* m_focused_hotkeys;
  ConfigBool* m_inhibit_screensaver;
  ConfigBool* m_play_time;

  ConfigBool* m_keep_on_top;
  ConfigBool* m_confirm_stop;
  ConfigBool* m_panic_handlers;
  ConfigBool* m_title_in_window;
  ConfigBool* m_pause_unfocused;
  ConfigRadioInt* m_cursor_on_movement;
  ConfigRadioInt* m_cursor_never;
  ConfigRadioInt* m_cursor_always;
  ConfigBool* m_lock_cursor;
};
