// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

class QDialogButtonBox;
class QPushButton;
class QTabWidget;
class QTableWidget;
class ToolTipCheckBox;

// Edits the SDL hints kept in the base layer's [SDL_Hints] section: a tab of
// common switches and a free-form name/value table.
class SDLHintsWindow final : public QDialog
{
  Q_OBJECT
public:
  explicit SDLHintsWindow(QWidget* parent);

  QSize sizeHint() const override;

private:
  enum Tab : int
  {
    TAB_LEAVING = -1,
    TAB_MAIN = 0,
    TAB_ADVANCED = 1,
  };

  QWidget* BuildAdvancedTab();
  QWidget* BuildMainTab();

  void LoadTable();
  void StoreTable();
  void LoadSwitches();
  void AppendRow();
  void DeleteCurrentRow();
  void UpdateRemoveButton();
  void Close();
  void SwitchTab(int next);
  void OnNameColumnResized(int column, int before, int after);

  bool eventFilter(QObject* watched, QEvent* event) override;

  QTabWidget* m_tabs;
  QDialogButtonBox* m_close_box;
  QTableWidget* m_table;
  QPushButton* m_remove_button;

  ToolTipCheckBox* m_directinput;
  ToolTipCheckBox* m_joycon_pairs;
  ToolTipCheckBox* m_joycon_sideways;
  ToolTipCheckBox* m_ps5_leds;

  int m_shown_tab;
};
