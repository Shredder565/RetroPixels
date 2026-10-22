// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include <QWidget>

namespace AudioCommon
{
enum class DPL2Quality;
}

class ConfigBool;
class ConfigChoice;
class ConfigComplexChoice;
class ConfigRadioBool;
class ConfigSlider;
class ConfigStringChoice;
class QFormLayout;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QRadioButton;
class SettingsWindow;

// "Audio" settings page: DSP engine, backend options, playback tweaks, the volume slider and
// (with Cubeb) per-Wii-Remote speaker routing.
class AudioPane final : public QWidget
{
  Q_OBJECT
public:
  explicit AudioPane();

private:
  QGroupBox* BuildDspBox();
  QGroupBox* BuildVolumeBox();
  QGroupBox* BuildBackendBox();
  QGroupBox* BuildPlaybackBox();
#ifdef HAVE_CUBEB
  QGroupBox* BuildRoutingBox();
#endif
  void SetTooltips();
  void WireUp();

  void OnEmulationStateChanged(bool running);
  void OnBackendChanged();
  void OnDspChanged();
  void RefreshRoutingState();

  // Whether any available backend lets the user set the latency.
  bool m_has_latency_setting = false;

  ConfigComplexChoice* m_dsp_engine;

  ConfigSlider* m_volume;
  QLabel* m_volume_value;

  QLabel* m_backend_label;
  ConfigStringChoice* m_backend;

  ConfigBool* m_dpl2;
  QLabel* m_dpl2_quality_label;
  ConfigChoice* m_dpl2_quality;

  QLabel* m_latency_label = nullptr;
  ConfigSlider* m_latency = nullptr;
#ifdef _WIN32
  QLabel* m_wasapi_device_label;
  ConfigStringChoice* m_wasapi_device;
#endif

  ConfigBool* m_fill_gaps;
  ConfigBool* m_preserve_pitch;
  ConfigBool* m_mute_when_unthrottled;

  QGroupBox* m_routing_box = nullptr;
  ConfigBool* m_routing = nullptr;
  std::array<ConfigBool*, 4> m_routed_remote{};
  std::array<ConfigStringChoice*, 4> m_routed_device{};
};
