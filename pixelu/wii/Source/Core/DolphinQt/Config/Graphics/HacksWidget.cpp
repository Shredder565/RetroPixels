// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Graphics/HacksWidget.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"
#include "DolphinQt/Config/GameConfigWidget.h"
#include "DolphinQt/Config/Graphics/GraphicsPane.h"

#include "VideoCommon/VideoConfig.h"

namespace
{
// Runs action whenever the box's check state changes (Qt 6.7 renamed the
// signal).
template <typename Action>
void OnCheckStateChange(QCheckBox* box, QObject* context, Action action)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  QObject::connect(box, &QCheckBox::checkStateChanged, context, [action](Qt::CheckState) {
    action();
  });
#else
  QObject::connect(box, &QCheckBox::stateChanged, context, [action](int) { action(); });
#endif
}
}  // namespace

HacksWidget::HacksWidget(GraphicsPane* gfx_pane) : m_game_layer{gfx_pane->GetConfigLayer()}
{
  auto* const column = new QVBoxLayout;
  QGroupBox* const groups[] = {BuildEfbGroup(), BuildTextureCacheGroup(), BuildXfbGroup(),
                               BuildOtherGroup()};
  for (QGroupBox* const group : groups)
    column->addWidget(group);
  column->addStretch();
  setLayout(column);

  RefreshDeferEfbCopies();
  RefreshSkipDuplicateFrames();

  WatchDependencies();
  SetDescriptions();

  // The backend's INI name, run through tr() for the "not supported" notes.
  const auto backend_name = [] { return tr(Config::Get(Config::MAIN_GFX_BACKEND).data()); };

  connect(gfx_pane, &GraphicsPane::BackendChanged, this, &HacksWidget::OnBackendChanged);
  connect(gfx_pane, &GraphicsPane::UpdateGPUTextureDecoding, this,
          [this, backend_name] { RefreshGpuTextureDecoding(backend_name()); });

  OnBackendChanged(backend_name());
}

QGroupBox* HacksWidget::BuildEfbGroup()
{
  auto* const box = new QGroupBox(tr("Embedded Frame Buffer (EFB)"));
  auto* const grid = new QGridLayout();
  box->setLayout(grid);

  // The first two are "skip"/"ignore" boxes over enable settings, so they
  // are inverted.
  const auto box_for = [this](const QString& label, const Config::Info<bool>& setting,
                              bool inverted) {
    return new ConfigBool(label, setting, m_game_layer, inverted);
  };
  m_skip_efb_cpu =
      box_for(tr("Skip EFB Access from CPU"), Config::GFX_HACK_EFB_ACCESS_ENABLE, true);
  m_ignore_format_changes =
      box_for(tr("Ignore Format Changes"), Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, true);
  m_store_efb_copies =
      box_for(tr("Store EFB Copies to Texture Only"), Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, false);
  m_defer_efb_copies =
      box_for(tr("Defer EFB Copies to RAM"), Config::GFX_HACK_DEFER_EFB_COPIES, false);

  grid->addWidget(m_skip_efb_cpu, 0, 0);
  grid->addWidget(m_ignore_format_changes, 0, 1);
  grid->addWidget(m_store_efb_copies, 1, 0);
  grid->addWidget(m_defer_efb_copies, 1, 1);
  return box;
}

QGroupBox* HacksWidget::BuildTextureCacheGroup()
{
  auto* const box = new QGroupBox(tr("Texture Cache"));
  auto* const grid = new QGridLayout();
  box->setLayout(grid);

  // Samples checked per texture: all (safe), 512, 128 (fast).
  m_accuracy =
      new ConfigSlider({0, 512, 128}, Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, m_game_layer);
  m_gpu_texture_decoding = new ConfigBool(tr("GPU Texture Decoding"),
                                          Config::GFX_ENABLE_GPU_TEXTURE_DECODING, m_game_layer);

  auto* const safe = new QLabel(tr("Safe"));
  safe->setAlignment(Qt::AlignRight);
  m_accuracy_label = new ConfigSliderLabel(tr("Accuracy:"), m_accuracy);

  grid->addWidget(m_accuracy_label, 0, 0);
  grid->addWidget(safe, 0, 1);
  grid->addWidget(m_accuracy, 0, 2);
  grid->addWidget(new QLabel(tr("Fast")), 0, 3);
  grid->addWidget(m_gpu_texture_decoding, 1, 0);
  return box;
}

QGroupBox* HacksWidget::BuildXfbGroup()
{
  auto* const box = new QGroupBox(tr("External Frame Buffer (XFB)"));
  auto* const list = new QVBoxLayout();
  box->setLayout(list);

  m_store_xfb_copies = new ConfigBool(tr("Store XFB Copies to Texture Only"),
                                      Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, m_game_layer);
  m_immediate_xfb =
      new ConfigBool(tr("Immediately Present XFB"), Config::GFX_HACK_IMMEDIATE_XFB, m_game_layer);
  m_skip_duplicate_xfbs = new ConfigBool(tr("Skip Presenting Duplicate Frames"),
                                         Config::GFX_HACK_SKIP_DUPLICATE_XFBS, m_game_layer);

  for (QWidget* const option : {static_cast<QWidget*>(m_store_xfb_copies),
                                static_cast<QWidget*>(m_immediate_xfb),
                                static_cast<QWidget*>(m_skip_duplicate_xfbs)})
  {
    list->addWidget(option);
  }
  return box;
}

QGroupBox* HacksWidget::BuildOtherGroup()
{
  auto* const box = new QGroupBox(tr("Other"));
  auto* const grid = new QGridLayout();
  box->setLayout(grid);

  const auto box_for = [this](const QString& label, const Config::Info<bool>& setting,
                              bool inverted) {
    return new ConfigBool(label, setting, m_game_layer, inverted);
  };
  m_fast_depth_calculation =
      box_for(tr("Fast Depth Calculation"), Config::GFX_FAST_DEPTH_CALC, false);
  // Stored as "bounding box enabled".
  m_disable_bounding_box = box_for(tr("Disable Bounding Box"), Config::GFX_HACK_BBOX_ENABLE, true);
  m_vertex_rounding = box_for(tr("Vertex Rounding"), Config::GFX_HACK_VERTEX_ROUNDING, false);
  m_save_texture_cache_state =
      box_for(tr("Save Texture Cache to State"), Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE, false);
  m_vi_skip = box_for(tr("VBI Skip"), Config::GFX_HACK_VI_SKIP, false);

  grid->addWidget(m_fast_depth_calculation, 0, 0);
  grid->addWidget(m_disable_bounding_box, 0, 1);
  grid->addWidget(m_vertex_rounding, 1, 0);
  grid->addWidget(m_save_texture_cache_state, 1, 1);
  grid->addWidget(m_vi_skip, 2, 0);
  return box;
}

void HacksWidget::WatchDependencies()
{
  OnCheckStateChange(m_store_efb_copies, this, [this] { RefreshDeferEfbCopies(); });
  OnCheckStateChange(m_store_xfb_copies, this, [this] { RefreshDeferEfbCopies(); });
  OnCheckStateChange(m_immediate_xfb, this, [this] { RefreshSkipDuplicateFrames(); });
  OnCheckStateChange(m_vi_skip, this, [this] { RefreshSkipDuplicateFrames(); });
}

void HacksWidget::OnBackendChanged(const QString& backend_name)
{
  RefreshGpuTextureDecoding(backend_name);
  RefreshBoundingBox(backend_name);
}

void HacksWidget::SetDescriptions()
{
  static const char skip_efb_help[] = QT_TR_NOOP(
      "Ignores any requests from the CPU to read from or write to the EFB. <br><br>"
      "Improves performance in some games, but will disable all EFB-based graphical effects or "
      "gameplay-related features.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char format_changes_help[] = QT_TR_NOOP(
      "Ignores any changes to the EFB format.<br><br>"
      "Improves performance in many games without any negative effect. Causes graphical defects "
      "in a small number of other games.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char efb_to_texture_help[] = QT_TR_NOOP(
      "Stores EFB copies exclusively on the GPU, bypassing system memory. Causes graphical "
      "defects in a small number of games.<br><br>"
      "Enabled = EFB Copies to Texture<br>"
      "Disabled = EFB Copies to RAM (and Texture)<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char defer_help[] = QT_TR_NOOP(
      "Waits until the game synchronizes with the emulated GPU before writing the contents of "
      "EFB copies to RAM.<br><br>"
      "Reduces the overhead of EFB RAM copies, providing a performance boost in many games, at "
      "the risk of breaking those which do not safely synchronize with the emulated GPU.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char accuracy_help[] = QT_TR_NOOP(
      "Adjusts the accuracy at which the GPU receives texture updates from RAM.<br><br>"
      "The \"Safe\" setting eliminates the likelihood of the GPU missing texture updates from "
      "RAM. Lower accuracies cause in-game text to appear garbled in certain games.<br><br>"
      "<dolphin_emphasis>If unsure, select the rightmost value.</dolphin_emphasis>");
  static const char xfb_to_texture_help[] = QT_TR_NOOP(
      "Stores XFB copies exclusively on the GPU, bypassing system memory. Causes graphical "
      "defects in a small number of games.<br><br>"
      "Enabled = XFB Copies to Texture<br>"
      "Disabled = XFB Copies to RAM (and Texture)<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char immediate_help[] = QT_TR_NOOP(
      "Displays XFB copies as soon as they are created, instead of waiting for scanout.<br><br>"
      "Can cause graphical defects in some games if the game doesn't expect all XFB copies to be "
      "displayed. However, turning this setting on reduces latency.<br><br>"
      "Enabling this also forces an effect equivalent to the Skip Presenting Duplicate Frames "
      "setting.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char skip_duplicate_help[] = QT_TR_NOOP(
      "Skips presentation of duplicate frames (XFB copies) in 25fps/30fps games. This may improve "
      "performance on low-end devices, while making frame pacing less consistent.<br><br>"
      "Disable this option for optimal frame pacing.<br><br>"
      "This setting is unavailable when Immediately Present XFB or VBI Skip is enabled. In those "
      "cases, duplicate frames are never presented.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char fast_depth_help[] = QT_TR_NOOP(
      "Uses a less accurate algorithm to calculate depth values.<br><br>"
      "Causes issues in a few games, but can result in a decent speed increase depending on the "
      "game and/or GPU.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char cache_to_state_help[] = QT_TR_NOOP(
      "Includes the contents of the embedded frame buffer (EFB) and upscaled EFB copies in save "
      "states. Fixes missing and/or non-upscaled textures/objects when loading states at the cost "
      "of additional save/load time.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char rounding_help[] = QT_TR_NOOP(
      "Rounds 2D vertices to whole pixels and rounds the viewport size to a whole number.<br><br>"
      "Fixes graphical problems in some games at higher internal resolutions. This setting has "
      "no effect when native internal resolution is used.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char vi_skip_help[] = QT_TR_NOOP(
      "Skips Vertical Blank Interrupts when lag is detected, allowing for smooth audio playback "
      "when emulation speed is not 100%. <br><br>"
      "Enabling this also forces the effect of the Skip Presenting Duplicate Frames "
      "setting.<br><br>"
      "<dolphin_emphasis>WARNING: Can cause freezes and compatibility issues.</dolphin_emphasis> "
      "<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");

  m_skip_efb_cpu->SetDescription(tr(skip_efb_help));
  m_ignore_format_changes->SetDescription(tr(format_changes_help));
  m_store_efb_copies->SetDescription(tr(efb_to_texture_help));
  m_defer_efb_copies->SetDescription(tr(defer_help));
  m_accuracy->SetTitle(tr("Texture Cache Accuracy"));
  m_accuracy->SetDescription(tr(accuracy_help));
  m_store_xfb_copies->SetDescription(tr(xfb_to_texture_help));
  m_immediate_xfb->SetDescription(tr(immediate_help));
  m_skip_duplicate_xfbs->SetDescription(tr(skip_duplicate_help));
  m_fast_depth_calculation->SetDescription(tr(fast_depth_help));
  m_save_texture_cache_state->SetDescription(tr(cache_to_state_help));
  m_vertex_rounding->SetDescription(tr(rounding_help));
  m_vi_skip->SetDescription(tr(vi_skip_help));
}

// GPU decoding needs backend support and is switched off by arbitrary
// mipmap detection; the description ends with whichever note applies.
void HacksWidget::RefreshGpuTextureDecoding(const QString& backend_name)
{
  static const char base_help[] = QT_TR_NOOP(
      "Enables texture decoding using the GPU instead of the CPU.<br><br>"
      "This may result in performance gains in some scenarios, or on systems where the CPU is the "
      "bottleneck.<br><br>"
      "This setting is disabled when Arbitrary Mipmap Detection is enabled.<br><br>");

  const bool supported = g_backend_info.bSupportsGPUTextureDecoding;
  const bool blocked_by_mipmaps = Get(m_game_layer, Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);
  m_gpu_texture_decoding->setEnabled(supported && !blocked_by_mipmaps);

  const QString help = tr(base_help);
  QString note;
  if (!supported)
  {
    note = tr("<dolphin_emphasis>The %1 backend doesn't support GPU Texture "
              "Decoding.</dolphin_emphasis>")
               .arg(backend_name);
  }
  else if (blocked_by_mipmaps)
  {
    note = tr("<dolphin_emphasis>GPU Texture Decoding is currently disabled by Arbitrary Mipmap "
              "Detection.</dolphin_emphasis>");
  }
  else
  {
    note = tr("<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  }
  m_gpu_texture_decoding->SetDescription(help + note);
}

void HacksWidget::RefreshBoundingBox(const QString& backend_name)
{
  static const char base_help[] = QT_TR_NOOP(
      "Disables bounding box emulation.<br><br>"
      "This may improve GPU performance significantly, but some games will break.<br><br>");

  const bool supported = g_backend_info.bSupportsBBox;
  m_disable_bounding_box->setEnabled(supported);

  const QString help = tr(base_help);
  const QString note =
      supported ?
          tr("<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>") :
          tr("<dolphin_emphasis>The %1 backend doesn't support Bounding Box "
             "emulation.</dolphin_emphasis>")
              .arg(backend_name);
  m_disable_bounding_box->SetDescription(help + note);
}

// Deferring is pointless once both EFB and XFB copies stay on the GPU.
void HacksWidget::RefreshDeferEfbCopies()
{
  const bool all_on_gpu = m_store_efb_copies->isChecked() && m_store_xfb_copies->isChecked();
  m_defer_efb_copies->setEnabled(!all_on_gpu);
}

// Immediate XFB and VBI skip never present a frame twice anyway.
void HacksWidget::RefreshSkipDuplicateFrames()
{
  const bool never_duplicates = m_immediate_xfb->isChecked() || m_vi_skip->isChecked();
  m_skip_duplicate_xfbs->setDisabled(never_duplicates);
}
