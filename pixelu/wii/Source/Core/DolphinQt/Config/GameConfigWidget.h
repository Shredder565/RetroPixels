// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>

#include <QString>
#include <QWidget>

namespace UICommon
{
class GameFile;
}

namespace Config
{
class Layer;
}  // namespace Config

class ConfigBool;
class ConfigFloatSlider;
class ConfigInteger;
class ConfigStringChoice;
class QPushButton;
class QTabWidget;

// The Game Config tab of the game properties: per-game core, stereoscopy and
// graphics overrides written to the user's game INI, plus raw editors for the
// default and user INI files.
class GameConfigWidget : public QWidget
{
  Q_OBJECT
public:
  explicit GameConfigWidget(const UICommon::GameFile& game);
  ~GameConfigWidget() override;

private:
  QWidget* BuildGeneralTab();
  QWidget* BuildEditorTab();
  void OnTabChanged(int index);
  void ReplaceUserIniEditor();
  void ShowGlobalValues();
  void MarkGlobalSettings();
  QString UserIniName() const;

  QString m_gameini_local_path;

  QTabWidget* m_default_inis;
  QTabWidget* m_user_inis;

  ConfigBool* m_dual_core;
  ConfigBool* m_mmu;
  ConfigBool* m_fprf;
  ConfigBool* m_sync_gpu;
  ConfigBool* m_disc_speed;
  ConfigBool* m_dsp_hle;
  ConfigBool* m_mono_shadows;

  ConfigStringChoice* m_determinism;
  ConfigFloatSlider* m_depth;
  ConfigFloatSlider* m_convergence;

  const UICommon::GameFile& m_game;
  std::string m_game_id;
  // The user INI layer the controls edit, and the read-only default INIs.
  std::unique_ptr<Config::Layer> m_layer;
  std::unique_ptr<Config::Layer> m_global_layer;
  int m_editor_tab = -1;
  int m_last_tab = 0;
};
