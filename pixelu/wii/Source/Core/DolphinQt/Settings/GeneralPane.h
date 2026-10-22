// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class ConfigBool;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QRadioButton;
class QSlider;
class QVBoxLayout;
class ToolTipCheckBox;
class ToolTipComboBox;
class ToolTipPushButton;

namespace Core
{
enum class State;
}

// "General" settings page: basic emulation toggles, speed limit, update track, fallback
// region and (when built in) usage statistics.
class GeneralPane final : public QWidget
{
  Q_OBJECT
public:
  explicit GeneralPane(QWidget* parent = nullptr);

private:
  QVBoxLayout* AddGroup(QVBoxLayout* page, const QString& title);
  void BuildBasicGroup(QVBoxLayout* page);
  void BuildUpdateGroup(QVBoxLayout* page);
  void BuildRegionGroup(QVBoxLayout* page);
  void SetTooltips();
  void WireUp();

  void ReadSettings();
  void WriteSettings();
  void OnEmulationStateChanged(Core::State state);
  void RefreshSpeedLimitTooltip();

  ConfigBool* m_dual_core;
  ConfigBool* m_cheats;
  ConfigBool* m_preload_game;
  ConfigBool* m_mismatched_region;
  ConfigBool* m_auto_disc_change;
#ifdef USE_DISCORD_PRESENCE
  ToolTipCheckBox* m_discord;
#endif
  ToolTipComboBox* m_speed_limit;
  ToolTipComboBox* m_update_track;
  ToolTipComboBox* m_fallback_region;

#if defined(USE_ANALYTICS) && USE_ANALYTICS
  void BuildStatisticsGroup(QVBoxLayout* page);
  void ResetStatisticsIdentity();

  ToolTipCheckBox* m_statistics;
  ToolTipPushButton* m_new_identity;
#endif
};
