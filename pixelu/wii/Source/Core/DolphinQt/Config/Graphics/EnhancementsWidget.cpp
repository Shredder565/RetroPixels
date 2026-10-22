// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Graphics/EnhancementsWidget.h"

#include <algorithm>
#include <utility>

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/CommonTypes.h"

#include "Core/Config/GraphicsSettings.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/GameConfigWidget.h"
#include "DolphinQt/Config/Graphics/ColorCorrectionConfigWindow.h"
#include "DolphinQt/Config/Graphics/GraphicsPane.h"
#include "DolphinQt/Config/Graphics/PostProcessingConfigWindow.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipPushButton.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"

#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr int AsInt(AnisotropicFilteringMode mode)
{
  return std::to_underlying(mode);
}
constexpr int AsInt(TextureFilteringMode mode)
{
  return std::to_underlying(mode);
}

bool UsesPerEyeResolution(StereoMode mode)
{
  return mode == StereoMode::SideBySide || mode == StereoMode::TopAndBottom;
}

// Stereo modes that only work with a matching post-processing shader.
bool NeedsStereoShader(StereoMode mode)
{
  return mode == StereoMode::Anaglyph || mode == StereoMode::Passive;
}
}  // namespace

EnhancementsWidget::EnhancementsWidget(GraphicsPane* gfx_pane)
    : m_game_layer{gfx_pane->GetConfigLayer()}
{
  auto* const column = new QVBoxLayout;
  QGroupBox* const enhancements = BuildEnhancementsGroup();
  QGroupBox* const stereoscopy = BuildStereoscopyGroup();
  column->addWidget(enhancements);
  column->addWidget(stereoscopy);
  column->addStretch();
  setLayout(column);

  RebuildPostProcessingChoices();

  connect(m_3d_mode, &QComboBox::currentIndexChanged, this,
          &EnhancementsWidget::OnStereoModeChanged);
  connect(m_post_processing_effect, &QComboBox::currentIndexChanged, this,
          &EnhancementsWidget::OnPostProcessingShaderChanged);
  connect(m_configure_color_correction, &QPushButton::clicked, this,
          &EnhancementsWidget::OpenColorCorrection);
  connect(m_configure_post_processing_effect, &QPushButton::clicked, this,
          &EnhancementsWidget::OpenPostProcessingOptions);
  connect(m_3d_depth, &ConfigFloatSlider::valueChanged, this, [this] {
    m_3d_depth_value->setText(QString::asprintf("%.0f", m_3d_depth->GetValue()));
  });
  connect(m_3d_convergence, &ConfigFloatSlider::valueChanged, this, [this] {
    m_3d_convergence_value->setText(QString::asprintf("%.2f", m_3d_convergence->GetValue()));
  });

  SetDescriptions();

  // The pane sends BackendChanged once while it is being set up.
  connect(gfx_pane, &GraphicsPane::BackendChanged, this, &EnhancementsWidget::OnBackendChanged);
  connect(gfx_pane, &GraphicsPane::UseFastTextureSamplingChanged, this, [this] {
    m_texture_filtering_combo->setEnabled(
        Get(m_game_layer, Config::GFX_HACK_FAST_TEXTURE_SAMPLING));
  });
  connect(m_arbitrary_mipmap_detection, &QCheckBox::toggled, gfx_pane,
          [gfx_pane] { emit gfx_pane->UpdateGPUTextureDecoding(); });
}

// "Auto", "Native", then one entry per integer scale from 2x up to the
// larger of the current and the maximum scale, so a hand-edited INI value
// above the usual range survives a save. Scales that first cover a common
// display height are labelled with it (720p for 2x, and so on); the labels
// ignore any black borders a game may draw.
QStringList EnhancementsWidget::ResolutionChoices() const
{
  QStringList choices{tr("Auto (Multiple of 640x528)"), tr("Native (640x528)")};
  const QString none = QStringLiteral("");
  const QStringList display_names{tr("720p"), tr("1080p"), tr("1440p"), none, tr("4K"), none,
                                  tr("5K"),   none,        none,         none, tr("8K")};

  const int highest = std::max(Get(m_game_layer, Config::GFX_EFB_SCALE),
                               Get(m_game_layer, Config::GFX_MAX_EFB_SCALE));
  for (int scale = static_cast<int>(choices.size()); scale <= highest; ++scale)
  {
    const QString factor = QString::number(scale);
    const QString width = QString::number(static_cast<int>(EFB_WIDTH) * scale);
    const QString height = QString::number(static_cast<int>(EFB_HEIGHT) * scale);
    const qsizetype name_index = choices.size() - 2;
    const QString display = name_index < display_names.size() ? display_names[name_index] : none;
    if (display.isEmpty())
      choices.append(tr("%1x Native (%2x%3)").arg(factor, width, height));
    else
      choices.append(tr("%1x Native (%2x%3) for %4").arg(factor, width, height, display));
  }
  return choices;
}

QGroupBox* EnhancementsWidget::BuildEnhancementsGroup()
{
  auto* const box = new QGroupBox(tr("Enhancements"));
  auto* const grid = new QGridLayout();
  box->setLayout(grid);

  // Enough rows to show Auto, Native and every named scale without scrolling.
  constexpr int visible_rows = 2 + 11;
  m_ir_combo = new ConfigChoice(ResolutionChoices(), Config::GFX_EFB_SCALE, m_game_layer);
  m_ir_combo->setMaxVisibleItems(visible_rows);

  m_antialiasing_combo = new ConfigComplexChoice(Config::GFX_MSAA, Config::GFX_SSAA, m_game_layer);
  m_antialiasing_combo->Add(tr("None"), static_cast<u32>(1), false);

  m_texture_filtering_combo =
      new ConfigComplexChoice(Config::GFX_ENHANCE_MAX_ANISOTROPY,
                              Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING, m_game_layer);
  {
    using Aniso = AnisotropicFilteringMode;
    const int game_filter = AsInt(TextureFilteringMode::Default);
    const int nearest = AsInt(TextureFilteringMode::Nearest);
    const int linear = AsInt(TextureFilteringMode::Linear);
    m_texture_filtering_combo->Add(tr("Default"), Config::DefaultState{}, game_filter);
    m_texture_filtering_combo->Add(tr("1x Anisotropic"), AsInt(Aniso::Force1x), game_filter);
    m_texture_filtering_combo->Add(tr("2x Anisotropic"), AsInt(Aniso::Force2x), game_filter);
    m_texture_filtering_combo->Add(tr("4x Anisotropic"), AsInt(Aniso::Force4x), game_filter);
    m_texture_filtering_combo->Add(tr("8x Anisotropic"), AsInt(Aniso::Force8x), game_filter);
    m_texture_filtering_combo->Add(tr("16x Anisotropic"), AsInt(Aniso::Force16x), game_filter);
    m_texture_filtering_combo->Add(tr("Force Nearest and 1x Anisotropic "), AsInt(Aniso::Force1x),
                                   nearest);
    m_texture_filtering_combo->Add(tr("Force Linear and 1x Anisotropic"), AsInt(Aniso::Force1x),
                                   linear);
    m_texture_filtering_combo->Add(tr("Force Linear and 2x Anisotropic"), AsInt(Aniso::Force2x),
                                   linear);
    m_texture_filtering_combo->Add(tr("Force Linear and 4x Anisotropic"), AsInt(Aniso::Force4x),
                                   linear);
    m_texture_filtering_combo->Add(tr("Force Linear and 8x Anisotropic"), AsInt(Aniso::Force8x),
                                   linear);
    m_texture_filtering_combo->Add(tr("Force Linear and 16x Anisotropic"), AsInt(Aniso::Force16x),
                                   linear);
  }
  m_texture_filtering_combo->Refresh();
  // Filtering is done by the shaders themselves with manual texture sampling.
  m_texture_filtering_combo->setEnabled(Get(m_game_layer, Config::GFX_HACK_FAST_TEXTURE_SAMPLING));

  m_output_resampling_combo = new ConfigChoice(
      {tr("Default"), tr("Bilinear"), tr("Bicubic: B-Spline"), tr("Bicubic: Mitchell-Netravali"),
       tr("Bicubic: Catmull-Rom"), tr("Sharp Bilinear"), tr("Area Sampling")},
      Config::GFX_ENHANCE_OUTPUT_RESAMPLING, m_game_layer);

  m_configure_color_correction = new ToolTipPushButton(tr("Configure"));

  // "(off)" stores an empty string, so the choice needs separate labels and
  // data. The list starts empty; RebuildPostProcessingChoices fills it.
  const std::vector<std::pair<QString, QString>> no_shaders_yet;
  m_post_processing_effect =
      new ConfigStringChoice(no_shaders_yet, Config::GFX_ENHANCE_POST_SHADER, m_game_layer);
  m_configure_post_processing_effect = new NonDefaultQPushButton(tr("Configure"));
  m_configure_post_processing_effect->setDisabled(true);

  // The plain on/off enhancements, all bound to the game layer.
  const auto toggle = [this](const QString& label, const Config::Info<bool>& setting) {
    return new ConfigBool(label, setting, m_game_layer);
  };
  m_scaled_efb_copy = toggle(tr("Scaled EFB Copy"), Config::GFX_HACK_COPY_EFB_SCALED);
  m_per_pixel_lighting = toggle(tr("Per-Pixel Lighting"), Config::GFX_ENABLE_PIXEL_LIGHTING);
  m_widescreen_hack = toggle(tr("Widescreen Hack"), Config::GFX_WIDESCREEN_HACK);
  m_disable_fog = toggle(tr("Disable Fog"), Config::GFX_DISABLE_FOG);
  m_force_24bit_color = toggle(tr("Force 24-Bit Color"), Config::GFX_ENHANCE_FORCE_TRUE_COLOR);
  m_disable_copy_filter =
      toggle(tr("Disable Copy Filter"), Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
  m_arbitrary_mipmap_detection =
      toggle(tr("Arbitrary Mipmap Detection"), Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);
  m_hdr = toggle(tr("HDR Post-Processing"), Config::GFX_ENHANCE_HDR_OUTPUT);

  // Captioned rows whose control spans the rest of the row.
  const std::pair<QString, QWidget*> captioned[] = {
      {tr("Internal Resolution:"), m_ir_combo},
      {tr("Anti-Aliasing:"), m_antialiasing_combo},
      {tr("Texture Filtering:"), m_texture_filtering_combo},
      {tr("Output Resampling:"), m_output_resampling_combo},
      {tr("Color Correction:"), m_configure_color_correction},
  };
  int row = 0;
  for (const auto& [caption, control] : captioned)
  {
    grid->addWidget(new QLabel(caption), row, 0);
    grid->addWidget(control, row, 1, 1, -1);
    ++row;
  }

  grid->addWidget(new QLabel(tr("Post-Processing Effect:")), row, 0);
  grid->addWidget(m_post_processing_effect, row, 1);
  grid->addWidget(m_configure_post_processing_effect, row, 2);
  ++row;

  // Check boxes, two per row.
  const std::pair<QWidget*, QWidget*> pairs[] = {
      {m_scaled_efb_copy, m_per_pixel_lighting},
      {m_widescreen_hack, m_force_24bit_color},
      {m_disable_fog, m_arbitrary_mipmap_detection},
      {m_disable_copy_filter, m_hdr},
  };
  for (const auto& [left, right] : pairs)
  {
    grid->addWidget(left, row, 0);
    grid->addWidget(right, row, 1, 1, -1);
    ++row;
  }

  return box;
}

QGroupBox* EnhancementsWidget::BuildStereoscopyGroup()
{
  auto* const box = new QGroupBox(tr("Stereoscopy"));
  auto* const grid = new QGridLayout();
  box->setLayout(grid);

  // Mode names in StereoMode order.
  const QStringList modes{tr("Off"),      tr("Side-by-Side"), tr("Top-and-Bottom"),
                          tr("Anaglyph"), tr("HDMI 3D"),      tr("Passive")};
  m_3d_mode = new ConfigChoice(modes, Config::GFX_STEREO_MODE, m_game_layer);

  // Depth moves in whole percent, convergence in hundredths.
  Config::Layer* const layer = m_game_layer;
  m_3d_depth = new ConfigFloatSlider(0, Config::GFX_STEREO_DEPTH_MAXIMUM, Config::GFX_STEREO_DEPTH,
                                     1.0f, layer);
  m_3d_convergence = new ConfigFloatSlider(0, Config::GFX_STEREO_CONVERGENCE_MAXIMUM,
                                           Config::GFX_STEREO_CONVERGENCE, 0.01f, layer);
  m_3d_depth_value = new QLabel();
  m_3d_convergence_value = new QLabel();
  m_3d_swap_eyes = new ConfigBool(tr("Swap Eyes"), Config::GFX_STEREO_SWAP_EYES, layer);
  m_3d_per_eye_resolution =
      new ConfigBool(tr("Use Full Resolution Per Eye"), Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL,
                     layer);

  grid->addWidget(new QLabel(tr("Stereoscopic 3D Mode:")), 0, 0);
  grid->addWidget(m_3d_mode, 0, 1);
  grid->addWidget(new ConfigFloatLabel(tr("Depth:"), m_3d_depth), 1, 0);
  grid->addWidget(m_3d_depth, 1, 1);
  grid->addWidget(m_3d_depth_value, 1, 2);
  grid->addWidget(new ConfigFloatLabel(tr("Convergence:"), m_3d_convergence), 2, 0);
  grid->addWidget(m_3d_convergence, 2, 1);
  grid->addWidget(m_3d_convergence_value, 2, 2);
  grid->addWidget(m_3d_swap_eyes, 3, 0);
  grid->addWidget(m_3d_per_eye_resolution, 4, 0);

  ShowStereoValues();
  if (!UsesPerEyeResolution(Get(m_game_layer, Config::GFX_STEREO_MODE)))
    m_3d_per_eye_resolution->hide();

  return box;
}

void EnhancementsWidget::ShowStereoValues()
{
  m_3d_depth_value->setText(QString::asprintf("%.0f", m_3d_depth->GetValue()));
  m_3d_convergence_value->setText(QString::asprintf("%.2f", m_3d_convergence->GetValue()));
}

void EnhancementsWidget::OnStereoModeChanged()
{
  // The mode combo has already stored the new mode.
  const StereoMode mode = Get(m_game_layer, Config::GFX_STEREO_MODE);
  RebuildPostProcessingChoices();
  m_3d_per_eye_resolution->setVisible(UsesPerEyeResolution(mode));
}

// Lists the shaders that fit the stereo mode. Anaglyph and passive 3D only
// offer their own shaders and must have one picked (dubois / horizontal by
// default), which is saved unless a game's INI is being edited.
void EnhancementsWidget::RebuildPostProcessingChoices()
{
  const StereoMode mode = Get(m_game_layer, Config::GFX_STEREO_MODE);

  const QSignalBlocker quiet(m_post_processing_effect);
  m_post_processing_effect->clear();

  std::vector<std::string> shaders = VideoCommon::PostProcessing::GetShaderList();
  if (mode == StereoMode::Anaglyph)
    shaders = VideoCommon::PostProcessing::GetAnaglyphShaderList();
  else if (mode == StereoMode::Passive)
    shaders = VideoCommon::PostProcessing::GetPassiveShaderList();

  if (!NeedsStereoShader(mode))
    m_post_processing_effect->addItem(tr("(off)"), QStringLiteral(""));

  std::string wanted = Get(m_game_layer, Config::GFX_ENHANCE_POST_SHADER);
  bool listed = false;
  for (const std::string& shader : shaders)
  {
    const QString name = QString::fromStdString(shader);
    m_post_processing_effect->addItem(name, name);
    if (shader == wanted)
    {
      m_post_processing_effect->setCurrentIndex(m_post_processing_effect->count() - 1);
      listed = true;
    }
  }

  if (!listed)
  {
    switch (mode)
    {
    case StereoMode::Anaglyph:
      wanted = "dubois";
      break;
    case StereoMode::Passive:
      wanted = "horizontal";
      break;
    default:
      wanted = "";
      break;
    }
    const int row = m_post_processing_effect->findData(QString::fromStdString(wanted));
    m_post_processing_effect->setCurrentIndex(std::max(0, row));

    if (m_game_layer == nullptr && Get(m_game_layer, Config::GFX_ENHANCE_POST_SHADER) != wanted)
      Config::SetBaseOrCurrent(Config::GFX_ENHANCE_POST_SHADER, wanted);
  }

  m_post_processing_effect->Load();
  OnPostProcessingShaderChanged();
}

void EnhancementsWidget::OnBackendChanged()
{
  const bool post_processing = g_backend_info.bSupportsPostProcessing;
  m_output_resampling_combo->setEnabled(post_processing);
  m_configure_color_correction->setEnabled(post_processing);
  m_hdr->setEnabled(g_backend_info.bSupportsHDROutput);

  const bool stereo = g_backend_info.bSupportsGeometryShaders;
  for (QWidget* const control :
       {static_cast<QWidget*>(m_3d_mode), static_cast<QWidget*>(m_3d_convergence),
        static_cast<QWidget*>(m_3d_depth), static_cast<QWidget*>(m_3d_swap_eyes)})
  {
    control->setEnabled(stereo);
  }

  if (!post_processing)
  {
    m_configure_post_processing_effect->setEnabled(false);
    m_post_processing_effect->setEnabled(false);
    m_post_processing_effect->setToolTip(
        tr("%1 doesn't support this feature.").arg(tr(g_video_backend->GetDisplayName().c_str())));
  }
  else if (!m_post_processing_effect->isEnabled())
  {
    // Coming back from a backend without post-processing.
    m_configure_post_processing_effect->setEnabled(true);
    m_post_processing_effect->setEnabled(true);
    m_post_processing_effect->setToolTip(QString{});
    RebuildPostProcessingChoices();
  }

  RebuildAntialiasingChoices();
}

void EnhancementsWidget::OnPostProcessingShaderChanged()
{
  std::string shader = Get(m_game_layer, Config::GFX_ENHANCE_POST_SHADER);

  if (shader.empty() || shader == "(off)")
  {
    shader.clear();
    // An empty shader in a game INI would not show in bold, so the key is
    // removed there instead of being set.
    if (m_game_layer == nullptr)
      Config::SetBaseOrCurrent(Config::GFX_ENHANCE_POST_SHADER, shader);
    else
      m_game_layer->DeleteKey(Config::GFX_ENHANCE_POST_SHADER.GetLocation());
  }

  bool has_options = false;
  if (!shader.empty() && m_post_processing_effect->isEnabled())
  {
    VideoCommon::PostProcessingConfiguration config;
    config.LoadShader(shader);
    has_options = config.HasOptions();
  }
  m_configure_post_processing_effect->setEnabled(has_options);
}

// None, then every MSAA level above 1x, then the same levels as SSAA when
// the backend has it.
void EnhancementsWidget::RebuildAntialiasingChoices()
{
  const QSignalBlocker quiet(m_antialiasing_combo);

  m_antialiasing_combo->Reset();
  m_antialiasing_combo->Add(tr("None"), static_cast<u32>(1), false);

  const std::vector<u32>& levels = g_backend_info.AAModes;
  for (const u32 level : levels)
  {
    if (level > 1)
      m_antialiasing_combo->Add(tr("%1x MSAA").arg(level), level, false);
  }
  if (g_backend_info.bSupportsSSAA)
  {
    for (const u32 level : levels)
    {
      if (level > 1)
        m_antialiasing_combo->Add(tr("%1x SSAA").arg(level), level, true);
    }
  }

  m_antialiasing_combo->Refresh();

  // The backend info describes the global backend. A game INI that picks a
  // different one may not support these levels, so editing is only allowed
  // when both agree.
  const bool same_backend =
      m_game_layer == nullptr || !m_game_layer->Exists(Config::MAIN_GFX_BACKEND.GetLocation()) ||
      Config::Get(Config::MAIN_GFX_BACKEND) == m_game_layer->Get(Config::MAIN_GFX_BACKEND);

  m_antialiasing_combo->setEnabled(m_antialiasing_combo->count() > 1 && same_backend);
}

void EnhancementsWidget::SetDescriptions()
{
  static const char ir_help[] = QT_TR_NOOP(
      "Controls the rendering resolution.<br><br>"
      "A high resolution greatly improves visual quality, but also greatly increases GPU load and "
      "can cause issues in certain games. Generally speaking, the lower the internal resolution, "
      "the better performance will be.<br><br>"
      "<dolphin_emphasis>If unsure, select Native.</dolphin_emphasis>");
  static const char aa_help[] = QT_TR_NOOP(
      "Reduces the amount of aliasing caused by rasterizing 3D graphics, resulting in smoother "
      "edges on objects. Increases GPU load and sometimes causes graphical issues.<br><br>"
      "SSAA is significantly more demanding than MSAA, but provides top quality geometry "
      "anti-aliasing and also applies anti-aliasing to lighting, shader effects, and "
      "textures.<br><br>"
      "<dolphin_emphasis>If unsure, select None.</dolphin_emphasis>");
  static const char filtering_help[] = QT_TR_NOOP(
      "Adjust the texture filtering. Anisotropic filtering enhances the visual quality of "
      "textures that are at oblique viewing angles. Force Nearest and Force Linear override the "
      "texture scaling filter selected by the game.<br><br>"
      "Any option except 'Default' will alter the look of the game's textures and might cause "
      "issues in a small number of games.<br><br>"
      "This setting is disabled when Manual Texture Sampling is enabled.<br><br>"
      "<dolphin_emphasis>If unsure, select 'Default'.</dolphin_emphasis>");
  static const char resampling_help[] = QT_TR_NOOP(
      "Affects how the game output is scaled to the window resolution.<br>"
      "The performance mostly depends on the number of samples each method uses.<br>"
      "Compared to SSAA, resampling is useful in case the output window<br>"
      "resolution isn't a multiplier of the native emulation resolution.<br><br>"
      "<b>Default</b> - [fastest]<br>"
      "Internal GPU bilinear sampler which is not gamma corrected.<br>"
      "This setting might be ignored if gamma correction is forced on.<br><br>"
      "<b>Bilinear</b> - [4 samples]<br>"
      "Gamma corrected linear interpolation between pixels.<br><br>"
      "<b>Bicubic</b> - [16 samples]<br>"
      "Gamma corrected cubic interpolation between pixels.<br>"
      "Good when rescaling between close resolutions, e.g. 1080p and 1440p.<br>"
      "Comes in various flavors:<br>"
      "<b>B-Spline</b>: Blurry, but avoids all lobing artifacts<br>"
      "<b>Mitchell-Netravali</b>: Good middle ground between blurry and lobing<br>"
      "<b>Catmull-Rom</b>: Sharper, but can cause lobing artifacts<br><br>"
      "<b>Sharp Bilinear</b> - [1-4 samples]<br>"
      "Similar to \"Nearest Neighbor\", it maintains a sharp look,<br>"
      "but also does some blending to avoid shimmering.<br>"
      "Works best with 2D games at low resolutions.<br><br>"
      "<b>Area Sampling</b> - [up to 324 samples]<br>"
      "Weighs pixels by the percentage of area they occupy. Gamma corrected.<br>"
      "Best for downscaling by more than 2x.<br><br>"
      "<dolphin_emphasis>If unsure, select 'Default'.</dolphin_emphasis>");
  static const char color_correction_help[] = QT_TR_NOOP(
      "A group of features to make the colors more accurate, matching the color space Wii and GC "
      "games were meant for.");
  static const char post_processing_help[] = QT_TR_NOOP(
      "Applies a post-processing effect after rendering a frame.<br><br />"
      "<dolphin_emphasis>If unsure, select (off).</dolphin_emphasis>");
  static const char scaled_efb_help[] = QT_TR_NOOP(
      "Greatly increases the quality of textures generated using render-to-texture effects.<br><br>"
      "Slightly increases GPU load and causes relatively few graphical issues. Raising the internal "
      "resolution will improve the effect of this setting. <br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char per_pixel_help[] = QT_TR_NOOP(
      "Calculates lighting of 3D objects per-pixel rather than per-vertex, smoothing out the "
      "appearance of lit polygons and making individual triangles less noticeable.<br><br />"
      "Rarely causes slowdowns or graphical issues.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char widescreen_help[] = QT_TR_NOOP(
      "Forces the game to output graphics at any aspect ratio by expanding the view frustum "
      "without stretching the image.<br>"
      "This is a hack, and its results will vary widely game to game (it often causes the UI to "
      "stretch).<br>"
      "Game-specific AR/Gecko-code aspect ratio patches are preferable over this if "
      "available.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char fog_help[] = QT_TR_NOOP(
      "Makes distant objects more visible by removing fog, thus increasing the overall "
      "detail.<br><br>"
      "Disabling fog will break some games which rely on proper fog emulation.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char stereo_mode_help[] = QT_TR_NOOP(
      "Selects the stereoscopic 3D mode. Stereoscopy allows a better feeling of depth if the "
      "necessary hardware is present. Heavily decreases emulation speed and sometimes causes "
      "issues.<br><br>"
      "Side-by-Side and Top-and-Bottom are used by most 3D TVs.<br>"
      "Anaglyph is used for Red-Cyan colored glasses.<br>"
      "HDMI 3D is used when the monitor supports 3D display resolutions.<br>"
      "Passive is another type of 3D used by some TVs.<br><br>"
      "<dolphin_emphasis>If unsure, select Off.</dolphin_emphasis>");
  static const char depth_help[] = QT_TR_NOOP(
      "Controls the separation distance between the virtual cameras.<br><br>"
      "A higher value creates a stronger feeling of depth while a lower value is more "
      "comfortable.");
  static const char convergence_help[] = QT_TR_NOOP(
      "Controls the distance of the convergence plane. This is the distance at which virtual "
      "objects will appear to be in front of the screen.<br><br>"
      "A higher value creates stronger out-of-screen effects while a lower value is more "
      "comfortable.");
  static const char swap_eyes_help[] = QT_TR_NOOP(
      "Swaps the left and right eye. Most useful in side-by-side stereoscopy mode.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char per_eye_help[] = QT_TR_NOOP(
      "Whether each eye gets full or half image resolution when using side-by-side or "
      "above-and-below 3D.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char true_color_help[] = QT_TR_NOOP(
      "Forces the game to render the RGB color channels in 24-bit, thereby increasing quality by "
      "reducing color banding.<br><br>"
      "Has no impact on performance and causes few graphical issues.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char copy_filter_help[] = QT_TR_NOOP(
      "Disables the blending of adjacent rows when copying the EFB. This is known in some games as "
      "\"deflickering\" or \"smoothing\".<br><br>"
      "Disabling the filter has no effect on performance, but may result in a sharper image. "
      "Causes few graphical issues.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char mipmap_help[] = QT_TR_NOOP(
      "Enables detection of arbitrary mipmaps, which some games use for special distance-based "
      "effects.<br><br>"
      "May have false positives that result in blurry textures at increased internal resolution, "
      "such as in games that use very low resolution mipmaps. Disabling this can also reduce "
      "stutter in games that frequently load new textures.<br><br>"
      "If this setting is enabled, GPU Texture Decoding will be disabled.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char hdr_help[] = QT_TR_NOOP(
      "Enables scRGB HDR output (if supported by your graphics backend and monitor). Fullscreen "
      "might be required.<br><br>"
      "This gives post process shaders more room for accuracy, allows \"AutoHDR\" post-process "
      "shaders to work, and allows to fully display the PAL and NTSC-J color spaces.<br><br>"
      "Note that games still render in SDR internally.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");

  m_ir_combo->SetTitle(tr("Internal Resolution"));
  m_ir_combo->SetDescription(tr(ir_help));
  m_antialiasing_combo->SetTitle(tr("Anti-Aliasing"));
  m_antialiasing_combo->SetDescription(tr(aa_help));
  m_texture_filtering_combo->SetTitle(tr("Texture Filtering"));
  m_texture_filtering_combo->SetDescription(tr(filtering_help));
  m_output_resampling_combo->SetTitle(tr("Output Resampling"));
  m_output_resampling_combo->SetDescription(tr(resampling_help));
  m_configure_color_correction->SetTitle(tr("Color Correction"));
  m_configure_color_correction->SetDescription(tr(color_correction_help));
  m_post_processing_effect->SetTitle(tr("Post-Processing Effect"));
  m_post_processing_effect->SetDescription(tr(post_processing_help));

  m_scaled_efb_copy->SetDescription(tr(scaled_efb_help));
  m_per_pixel_lighting->SetDescription(tr(per_pixel_help));
  m_widescreen_hack->SetDescription(tr(widescreen_help));
  m_disable_fog->SetDescription(tr(fog_help));
  m_force_24bit_color->SetDescription(tr(true_color_help));
  m_disable_copy_filter->SetDescription(tr(copy_filter_help));
  m_arbitrary_mipmap_detection->SetDescription(tr(mipmap_help));
  m_hdr->SetDescription(tr(hdr_help));

  m_3d_mode->SetTitle(tr("Stereoscopic 3D Mode"));
  m_3d_mode->SetDescription(tr(stereo_mode_help));
  m_3d_depth->SetTitle(tr("Depth"));
  m_3d_depth->SetDescription(tr(depth_help));
  m_3d_convergence->SetTitle(tr("Convergence"));
  m_3d_convergence->SetDescription(tr(convergence_help));
  m_3d_per_eye_resolution->SetDescription(tr(per_eye_help));
  m_3d_swap_eyes->SetDescription(tr(swap_eyes_help));
}

void EnhancementsWidget::OpenColorCorrection()
{
  ColorCorrectionConfigWindow dialog(this);
  dialog.exec();
}

void EnhancementsWidget::OpenPostProcessingOptions()
{
  const std::string shader = Get(m_game_layer, Config::GFX_ENHANCE_POST_SHADER);
  PostProcessingConfigWindow dialog(this, shader);
  dialog.exec();
}
