// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class ConfigBool;
class ConfigChoice;
class ConfigComplexChoice;
class ConfigStringChoice;
class ConfigFloatSlider;
class GraphicsPane;
class QGroupBox;
class QPushButton;
class QLabel;
class ToolTipPushButton;

namespace Config
{
template <typename T>
class Info;
class Layer;
}  // namespace Config

// Graphics > Enhancements: resolution, anti-aliasing, filtering,
// resampling, colour correction, post-processing and stereoscopy.
class EnhancementsWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit EnhancementsWidget(GraphicsPane* gfx_pane);

private:
  QGroupBox* BuildEnhancementsGroup();
  QGroupBox* BuildStereoscopyGroup();
  QStringList ResolutionChoices() const;
  void SetDescriptions();

  void OnBackendChanged();
  void OnStereoModeChanged();
  void RebuildAntialiasingChoices();
  void RebuildPostProcessingChoices();
  void OnPostProcessingShaderChanged();
  void ShowStereoValues();

  void OpenColorCorrection();
  void OpenPostProcessingOptions();

  ConfigChoice* m_ir_combo;
  ConfigComplexChoice* m_antialiasing_combo;
  ConfigComplexChoice* m_texture_filtering_combo;
  ConfigChoice* m_output_resampling_combo;
  ConfigStringChoice* m_post_processing_effect;
  ToolTipPushButton* m_configure_color_correction;
  QPushButton* m_configure_post_processing_effect;
  ConfigBool* m_scaled_efb_copy;
  ConfigBool* m_per_pixel_lighting;
  ConfigBool* m_widescreen_hack;
  ConfigBool* m_disable_fog;
  ConfigBool* m_force_24bit_color;
  ConfigBool* m_disable_copy_filter;
  ConfigBool* m_arbitrary_mipmap_detection;
  ConfigBool* m_hdr;

  ConfigChoice* m_3d_mode;
  ConfigFloatSlider* m_3d_depth;
  QLabel* m_3d_depth_value;
  ConfigFloatSlider* m_3d_convergence;
  QLabel* m_3d_convergence_value;
  ConfigBool* m_3d_swap_eyes;
  ConfigBool* m_3d_per_eye_resolution;

  Config::Layer* m_game_layer = nullptr;
};
