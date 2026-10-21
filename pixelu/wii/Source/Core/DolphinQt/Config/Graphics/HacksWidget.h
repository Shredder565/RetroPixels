// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class ConfigBool;
class ConfigSlider;
class ConfigSliderLabel;
class GraphicsPane;
class QGroupBox;
class QString;

namespace Config
{
class Layer;
}  // namespace Config

// Graphics > Hacks: EFB, texture cache, XFB and other speed hacks.
class HacksWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit HacksWidget(GraphicsPane* gfx_pane);

private:
  QGroupBox* BuildEfbGroup();
  QGroupBox* BuildTextureCacheGroup();
  QGroupBox* BuildXfbGroup();
  QGroupBox* BuildOtherGroup();
  void WatchDependencies();
  void SetDescriptions();

  void OnBackendChanged(const QString& backend_name);
  void RefreshGpuTextureDecoding(const QString& backend_name);
  void RefreshBoundingBox(const QString& backend_name);
  void RefreshDeferEfbCopies();
  void RefreshSkipDuplicateFrames();

  ConfigBool* m_skip_efb_cpu;
  ConfigBool* m_ignore_format_changes;
  ConfigBool* m_store_efb_copies;
  ConfigBool* m_defer_efb_copies;

  ConfigSliderLabel* m_accuracy_label;
  ConfigSlider* m_accuracy;
  ConfigBool* m_gpu_texture_decoding;

  ConfigBool* m_store_xfb_copies;
  ConfigBool* m_immediate_xfb;
  ConfigBool* m_skip_duplicate_xfbs;

  ConfigBool* m_fast_depth_calculation;
  ConfigBool* m_disable_bounding_box;
  ConfigBool* m_vertex_rounding;
  ConfigBool* m_vi_skip;
  ConfigBool* m_save_texture_cache_state;

  Config::Layer* m_game_layer = nullptr;
};
