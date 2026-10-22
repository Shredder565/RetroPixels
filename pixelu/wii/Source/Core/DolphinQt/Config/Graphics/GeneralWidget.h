// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include <QWidget>

class ConfigBool;
class ConfigChoice;
class ConfigInteger;
class ConfigRadioInt;
class ConfigStringChoice;
class GraphicsPane;
class QGroupBox;
class QLabel;
class ToolTipComboBox;

namespace Config
{
class Layer;
}  // namespace Config

// Graphics > General: backend, adapter, aspect ratio, window options and
// the shader compilation mode.
class GeneralWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit GeneralWidget(GraphicsPane* gfx_pane);

signals:
  void BackendChanged(const QString& backend);

private:
  QGroupBox* BuildBasicGroup();
  QGroupBox* BuildOtherGroup();
  QGroupBox* BuildShaderGroup();
  void SetDescriptions();

  void ConfirmBackendChoice();
  void ShowCustomAspectFields(int aspect_index);
  void FillAdapterList(const QString& backend_name);
  void OnEmulationStateChanged(bool running);

  ConfigStringChoice* m_backend_combo;
  ToolTipComboBox* m_adapter_combo;
  ConfigChoice* m_aspect_combo;
  QLabel* m_custom_aspect_label;
  ConfigInteger* m_custom_aspect_width;
  ConfigInteger* m_custom_aspect_height;
  ConfigBool* m_enable_vsync;
  ConfigBool* m_enable_fullscreen;

  ConfigBool* m_autoadjust_window_size;
  ConfigBool* m_render_main_window;

  std::array<ConfigRadioInt*, 4> m_shader_modes{};
  ConfigBool* m_wait_for_shaders;
  // Index to return to when a backend change is declined.
  int m_last_backend_index = 0;
  Config::Layer* m_game_layer = nullptr;
};
