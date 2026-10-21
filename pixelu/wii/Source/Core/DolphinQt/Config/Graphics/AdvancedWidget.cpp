// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Graphics/AdvancedWidget.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigInteger.h"
#include "DolphinQt/Config/GameConfigWidget.h"
#include "DolphinQt/Config/Graphics/GraphicsPane.h"
#include "DolphinQt/Settings.h"

#include "VideoCommon/VideoConfig.h"

namespace
{
template <typename Layout>
QGroupBox* NewGroup(const QString& title, Layout*& layout)
{
  auto* const box = new QGroupBox(title);
  layout = new Layout();
  box->setLayout(layout);
  return box;
}
}  // namespace

AdvancedWidget::AdvancedWidget(GraphicsPane* gfx_pane) : m_game_layer{gfx_pane->GetConfigLayer()}
{
  auto* const column = new QVBoxLayout;
  QGroupBox* const groups[] = {BuildDebuggingGroup(),   BuildUtilityGroup(), BuildTextureDumpGroup(),
                               BuildFrameDumpGroup(),   BuildCropGroup(),    BuildMiscGroup(),
                               BuildExperimentalGroup()};
  for (QGroupBox* const group : groups)
    column->addWidget(group);
  column->addStretch();
  setLayout(column);

  LinkDependentOptions();
  SetDescriptions();

  connect(gfx_pane, &GraphicsPane::BackendChanged, this, &AdvancedWidget::OnBackendChanged);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    OnEmulationStateChanged(state != Core::State::Uninitialized);
  });
  connect(m_manual_texture_sampling, &QCheckBox::toggled, gfx_pane,
          [gfx_pane] { emit gfx_pane->UseFastTextureSamplingChanged(); });

  OnBackendChanged();
  OnEmulationStateChanged(!Core::IsUninitialized(Core::System::GetInstance()));
}

QGroupBox* AdvancedWidget::BuildDebuggingGroup()
{
  QGridLayout* grid;
  QGroupBox* const box = NewGroup(tr("Debugging"), grid);

  m_log_render_time = new ConfigBool(tr("Log Render Time to File"),
                                     Config::GFX_LOG_RENDER_TIME_TO_FILE, m_game_layer);
  m_enable_wireframe =
      new ConfigBool(tr("Enable Wireframe"), Config::GFX_ENABLE_WIREFRAME, m_game_layer);
  m_enable_format_overlay =
      new ConfigBool(tr("Texture Format Overlay"), Config::GFX_TEXFMT_OVERLAY_ENABLE, m_game_layer);
  m_enable_api_validation = new ConfigBool(tr("Enable API Validation Layers"),
                                           Config::GFX_ENABLE_VALIDATION_LAYER, m_game_layer);

  grid->addWidget(m_enable_wireframe, 0, 0);
  grid->addWidget(m_enable_format_overlay, 0, 1);
  grid->addWidget(m_enable_api_validation, 1, 0);
  grid->addWidget(m_log_render_time, 1, 1);
  return box;
}

QGroupBox* AdvancedWidget::BuildUtilityGroup()
{
  QGridLayout* grid;
  QGroupBox* const box = NewGroup(tr("Utility"), grid);

  m_load_custom_textures =
      new ConfigBool(tr("Load Custom Textures"), Config::GFX_HIRES_TEXTURES, m_game_layer);
  m_prefetch_custom_textures = new ConfigBool(tr("Prefetch Custom Textures"),
                                              Config::GFX_CACHE_HIRES_TEXTURES, m_game_layer);
  m_prefetch_custom_textures->setEnabled(m_load_custom_textures->isChecked());

  // Dump targets are global only: a game INI that dumps would be a nasty
  // surprise.
  m_dump_efb_target = new ConfigBool(tr("Dump EFB Target"), Config::GFX_DUMP_EFB_TARGET);
  m_dump_xfb_target = new ConfigBool(tr("Dump XFB Target"), Config::GFX_DUMP_XFB_TARGET);
  if (m_game_layer != nullptr)
  {
    m_dump_efb_target->setEnabled(false);
    m_dump_xfb_target->setEnabled(false);
  }

  m_disable_vram_copies = new ConfigBool(tr("Disable EFB VRAM Copies"),
                                         Config::GFX_HACK_DISABLE_COPY_TO_VRAM, m_game_layer);
  m_enable_graphics_mods =
      new ConfigBool(tr("Enable Graphics Mods"), Config::GFX_MODS_ENABLE, m_game_layer);

  grid->addWidget(m_load_custom_textures, 0, 0);
  grid->addWidget(m_prefetch_custom_textures, 0, 1);
  grid->addWidget(m_disable_vram_copies, 1, 0);
  grid->addWidget(m_enable_graphics_mods, 1, 1);
  grid->addWidget(m_dump_efb_target, 2, 0);
  grid->addWidget(m_dump_xfb_target, 2, 1);
  return box;
}

QGroupBox* AdvancedWidget::BuildTextureDumpGroup()
{
  QGridLayout* grid;
  QGroupBox* const box = NewGroup(tr("Texture Dumping"), grid);

  // Global only, like the dump targets.
  m_dump_textures = new ConfigBool(tr("Enable"), Config::GFX_DUMP_TEXTURES);
  m_dump_base_textures = new ConfigBool(tr("Dump Base Textures"), Config::GFX_DUMP_BASE_TEXTURES);
  m_dump_mip_textures = new ConfigBool(tr("Dump Mip Maps"), Config::GFX_DUMP_MIP_TEXTURES);
  const bool dumping = m_dump_textures->isChecked();
  m_dump_mip_textures->setEnabled(dumping);
  m_dump_base_textures->setEnabled(dumping);
  if (m_game_layer != nullptr)
  {
    for (QWidget* const option : {static_cast<QWidget*>(m_dump_textures),
                                  static_cast<QWidget*>(m_dump_base_textures),
                                  static_cast<QWidget*>(m_dump_mip_textures)})
    {
      option->setEnabled(false);
    }
  }

  grid->addWidget(m_dump_textures, 0, 0);
  grid->addWidget(m_dump_base_textures, 1, 0);
  grid->addWidget(m_dump_mip_textures, 1, 1);
  return box;
}

QGroupBox* AdvancedWidget::BuildFrameDumpGroup()
{
  QGridLayout* grid;
  QGroupBox* const box = NewGroup(tr("Frame Dumping"), grid);

  m_frame_dumps_resolution_type =
      new ConfigChoice({tr("Window Resolution"), tr("Aspect Ratio Corrected Internal Resolution"),
                        tr("Raw Internal Resolution")},
                       Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE, m_game_layer);
  m_png_compression_level =
      new ConfigInteger(0, 9, Config::GFX_PNG_COMPRESSION_LEVEL, m_game_layer);
  grid->addWidget(new QLabel(tr("Resolution Type:")), 0, 0);
  grid->addWidget(m_frame_dumps_resolution_type, 0, 1);

#if defined(HAVE_FFMPEG)
  // Video dumps: a lossless codec, or a bitrate for the lossy one.
  m_dump_use_lossless =
      new ConfigBool(tr("Use Lossless Codec (Ut Video)"), Config::GFX_USE_LOSSLESS, m_game_layer);
  m_dump_bitrate = new ConfigInteger(0, 1000000, Config::GFX_BITRATE_KBPS, m_game_layer, 1000);
  m_dump_bitrate->setEnabled(!m_dump_use_lossless->isChecked());

  grid->addWidget(m_dump_use_lossless, 1, 0);
  grid->addWidget(new QLabel(tr("Bitrate (kbps):")), 2, 0);
  grid->addWidget(m_dump_bitrate, 2, 1);
#endif

  grid->addWidget(new QLabel(tr("PNG Compression Level:")), 3, 0);
  m_png_compression_level->SetTitle(tr("PNG Compression Level"));
  grid->addWidget(m_png_compression_level, 3, 1);
  return box;
}

QGroupBox* AdvancedWidget::BuildCropGroup()
{
  QVBoxLayout* list;
  QGroupBox* const box = NewGroup(tr("Crop"), list);

  m_crop_to_aspect_ratio =
      new ConfigBool(tr("To Aspect Ratio"), Config::GFX_CROP_TO_ASPECT_RATIO, m_game_layer);
  m_crop_custom = new ConfigBool(tr("Custom"), Config::GFX_CROP_CUSTOM, m_game_layer);

  QGridLayout* edges;
  m_crop_custom_box = NewGroup(tr("Custom"), edges);
  m_crop_custom_box->setDisabled(!m_crop_custom->isChecked());

  // Native pixels to remove from each edge of the 640x528 picture.
  m_crop_custom_left = new ConfigInteger(0, 640, Config::GFX_CROP_CUSTOM_LEFT, m_game_layer, 1);
  auto* const left_label = new ConfigIntegerLabel(tr("Left"), m_crop_custom_left);
  m_crop_custom_top = new ConfigInteger(0, 528, Config::GFX_CROP_CUSTOM_TOP, m_game_layer, 1);
  auto* const top_label = new ConfigIntegerLabel(tr("Top"), m_crop_custom_top);
  m_crop_custom_right = new ConfigInteger(0, 640, Config::GFX_CROP_CUSTOM_RIGHT, m_game_layer, 1);
  auto* const right_label = new ConfigIntegerLabel(tr("Right"), m_crop_custom_right);
  m_crop_custom_bottom = new ConfigInteger(0, 528, Config::GFX_CROP_CUSTOM_BOTTOM, m_game_layer, 1);
  auto* const bottom_label = new ConfigIntegerLabel(tr("Bottom"), m_crop_custom_bottom);

  // Two edges per row: label then field.
  const std::pair<QWidget*, QWidget*> cells[] = {{left_label, m_crop_custom_left},
                                                 {top_label, m_crop_custom_top},
                                                 {right_label, m_crop_custom_right},
                                                 {bottom_label, m_crop_custom_bottom}};
  for (int i = 0; i < 4; ++i)
  {
    const int row = i / 2;
    const int col = (i % 2) * 2;
    edges->addWidget(cells[i].first, row, col);
    edges->addWidget(cells[i].second, row, col + 1);
  }

  list->addWidget(m_crop_to_aspect_ratio);
  list->addWidget(m_crop_custom);
  list->addWidget(m_crop_custom_box);
  return box;
}

QGroupBox* AdvancedWidget::BuildMiscGroup()
{
  QGridLayout* grid;
  QGroupBox* const box = NewGroup(tr("Misc"), grid);

  m_enable_prog_scan =
      new ConfigBool(tr("Enable Progressive Scan"), Config::SYSCONF_PROGRESSIVE_SCAN, m_game_layer);
  m_backend_multithreading = new ConfigBool(tr("Backend Multithreading"),
                                            Config::GFX_BACKEND_MULTITHREADING, m_game_layer);
  // i18n: "VS" stands for vertex shader(s).
  m_prefer_vs_for_point_line_expansion =
      new ConfigBool(tr("Prefer VS for Point/Line Expansion"),
                     Config::GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION, m_game_layer);
  m_cpu_cull = new ConfigBool(tr("Cull Vertices on the CPU"), Config::GFX_CPU_CULL, m_game_layer);

  grid->addWidget(m_backend_multithreading, 0, 0);
  grid->addWidget(m_enable_prog_scan, 0, 1);
  grid->addWidget(m_cpu_cull, 1, 0);
  grid->addWidget(m_prefer_vs_for_point_line_expansion, 1, 1);

#ifdef _WIN32
  m_borderless_fullscreen =
      new ConfigBool(tr("Borderless Fullscreen"), Config::GFX_BORDERLESS_FULLSCREEN, m_game_layer);
  grid->addWidget(m_borderless_fullscreen, 2, 1);
#endif
  return box;
}

QGroupBox* AdvancedWidget::BuildExperimentalGroup()
{
  QGridLayout* grid;
  QGroupBox* const box = NewGroup(tr("Experimental"), grid);

  m_defer_efb_access_invalidation = new ConfigBool(
      tr("Defer EFB Cache Invalidation"), Config::GFX_HACK_EFB_DEFER_INVALIDATION, m_game_layer);
  // "Manual" sampling is the fast-sampling hack switched off.
  m_manual_texture_sampling = new ConfigBool(
      tr("Manual Texture Sampling"), Config::GFX_HACK_FAST_TEXTURE_SAMPLING, m_game_layer, true);

  grid->addWidget(m_defer_efb_access_invalidation, 0, 0);
  grid->addWidget(m_manual_texture_sampling, 0, 1);
  return box;
}

void AdvancedWidget::LinkDependentOptions()
{
  connect(m_load_custom_textures, &QCheckBox::toggled, this,
          [this](bool on) { m_prefetch_custom_textures->setEnabled(on); });
  connect(m_dump_textures, &QCheckBox::toggled, this, [this](bool on) {
    m_dump_mip_textures->setEnabled(on);
    m_dump_base_textures->setEnabled(on);
  });
  connect(m_enable_graphics_mods, &QCheckBox::toggled, this,
          [](bool on) { emit Settings::Instance().EnableGfxModsChanged(on); });
  connect(m_crop_custom, &QCheckBox::toggled, this,
          [this](bool on) { m_crop_custom_box->setDisabled(!on); });
#if defined(HAVE_FFMPEG)
  connect(m_dump_use_lossless, &QCheckBox::toggled, this,
          [this](bool on) { m_dump_bitrate->setEnabled(!on); });
#endif
}

void AdvancedWidget::OnBackendChanged()
{
  m_backend_multithreading->setEnabled(g_backend_info.bSupportsMultithreading);
  m_prefer_vs_for_point_line_expansion->setEnabled(g_backend_info.bSupportsGeometryShaders &&
                                                   g_backend_info.bSupportsVSLinePointExpand);
  // The VS expansion note names the backend.
  SetDescriptions();
}

void AdvancedWidget::OnEmulationStateChanged(bool running)
{
  m_enable_prog_scan->setEnabled(!running);
}

void AdvancedWidget::SetDescriptions()
{
  static const char wireframe_help[] = QT_TR_NOOP(
      "Renders the scene as a wireframe.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char format_overlay_help[] = QT_TR_NOOP(
      "Modifies textures to show the format they're encoded in.<br><br>"
      "May require an emulation reset to apply.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char validation_help[] = QT_TR_NOOP(
      "Enables validation of API calls made by the video backend, which may assist in debugging "
      "graphical issues. On the Vulkan and D3D backends, this also enables debug symbols for the "
      "compiled shaders.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char render_time_help[] = QT_TR_NOOP(
      "Logs the render time of every frame to User/Logs/render_time.txt.<br><br>"
      "Use this feature to measure Dolphin's performance.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char dump_textures_help[] = QT_TR_NOOP(
      "Dumps decoded game textures based on the other flags to "
      "User/Dump/Textures/&lt;game_id&gt;/.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char dump_mips_help[] = QT_TR_NOOP(
      "Whether to dump mipmapped game textures to User/Dump/Textures/&lt;game_id&gt;/.  This "
      "includes arbitrary mipmapped textures if 'Arbitrary Mipmap Detection' is enabled in "
      "Enhancements.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char dump_base_help[] = QT_TR_NOOP(
      "Whether to dump base game textures to User/Dump/Textures/&lt;game_id&gt;/.  This includes "
      "arbitrary base textures if 'Arbitrary Mipmap Detection' is enabled in "
      "Enhancements.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char load_custom_help[] = QT_TR_NOOP(
      "Loads custom textures from User/Load/Textures/&lt;game_id&gt;/ and "
      "User/Load/DynamicInputTextures/&lt;game_id&gt;/.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char prefetch_help[] = QT_TR_NOOP(
      "Caches custom textures to system RAM on startup.<br><br>"
      "This can require exponentially more RAM but fixes possible stuttering.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char dump_efb_help[] = QT_TR_NOOP(
      "Dumps the contents of EFB copies to User/Dump/Textures/.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char dump_xfb_help[] = QT_TR_NOOP(
      "Dumps the contents of XFB copies to User/Dump/Textures/.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char vram_copies_help[] = QT_TR_NOOP(
      "Disables the VRAM copy of the EFB, forcing a round-trip to RAM. Inhibits all "
      "upscaling.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char graphics_mods_help[] = QT_TR_NOOP(
      "Loads graphics mods from User/Load/GraphicsMods/.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char resolution_type_help[] = QT_TR_NOOP(
      "Selects how frame dumps (videos) and screenshots are going to be captured.<br>"
      "If the game or window resolution change during a recording, multiple video files might be "
      "created.<br>"
      "Note that color correction and cropping are always ignored by the captures.<br><br>"
      "<b>Window Resolution</b>: Uses the output window resolution (without black bars).<br>"
      "This is a simple dumping option that will capture the image more or less as you see it.<br>"
      "<b>Aspect Ratio Corrected Internal Resolution</b>: Uses the Internal Resolution (XFB size), "
      "and corrects it by the target aspect ratio.<br>"
      "This option will consistently dump at the specified Internal Resolution regardless of how "
      "the image is displayed during recording.<br>"
      "<b>Raw Internal Resolution</b>: Uses the Internal Resolution (XFB size) without correcting "
      "it with the target aspect ratio.<br>"
      "This will provide a clean dump without any aspect ratio correction so users have as raw as "
      "possible input for external editing software.<br><br>"
      "<dolphin_emphasis>If unsure, leave this at \"Aspect Ratio Corrected Internal "
      "Resolution\".</dolphin_emphasis>");
#if defined(HAVE_FFMPEG)
  static const char lossless_help[] = QT_TR_NOOP(
      "Encodes frame dumps using the Ut Video codec. If this option is unchecked, a lossy Xvid "
      "codec will be used.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
#endif
  static const char png_level_help[] = QT_TR_NOOP(
      "Specifies the zlib compression level to use when saving PNG images (both for screenshots "
      "and framedumping).<br><br>"
      "Since PNG uses lossless compression, this does not affect the image quality; instead, it is "
      "a trade-off between file size and compression time.<br><br>"
      "A value of 0 uses no compression at all.  A value of 1 uses very little compression, while "
      "the maximum value of 9 applies a lot of compression.  However, for PNG files, levels "
      "between 3 and 6 are generally about as good as level 9 but finish in significantly less "
      "time.<br><br>"
      "<dolphin_emphasis>If unsure, leave this at 6.</dolphin_emphasis>");
  static const char prog_scan_help[] = QT_TR_NOOP(
      "Enables progressive scan if supported by the emulated software. Most games don't have any "
      "issue with this.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char multithreading_help[] = QT_TR_NOOP(
      "Enables multithreaded command submission in backends where supported. Enabling this option "
      "may result in a performance improvement on systems with more than two CPU cores. "
      "Currently, this is limited to the Vulkan backend.<br><br>"
      "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>");
  static const char vs_expansion_help[] = QT_TR_NOOP(
      "On backends that support both using the geometry shader and the vertex shader for "
      "expanding points and lines, selects the vertex shader for the job.  May affect "
      "performance.<br><br>%1");
  static const char cpu_cull_help[] = QT_TR_NOOP(
      "Cull vertices on the CPU to reduce the number of draw calls required.  May affect "
      "performance and draw statistics.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char defer_invalidation_help[] = QT_TR_NOOP(
      "Defers invalidation of the EFB access cache until a GPU synchronization command is "
      "executed. If disabled, the cache will be invalidated with every draw call. <br><br>"
      "May improve performance in some games which rely on CPU EFB Access at the cost of "
      "stability.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char manual_sampling_help[] = QT_TR_NOOP(
      "Use a manual implementation of texture sampling instead of the graphics backend's "
      "built-in functionality.<br><br>"
      "This setting can fix graphical issues in some games on certain GPUs, most commonly "
      "vertical lines on FMVs. In addition to this, enabling Manual Texture Sampling will allow "
      "for correct emulation of texture wrapping special cases (at 1x IR or when scaled EFB is "
      "disabled, and with custom textures disabled) and better emulates Level of Detail "
      "calculation.<br><br>"
      "This comes at the cost of potentially worse performance, especially at higher internal "
      "resolutions.<br><br>"
      "If this setting is enabled, the Texture Filtering setting will be disabled.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
#ifdef _WIN32
  static const char borderless_help[] = QT_TR_NOOP(
      "Implements fullscreen mode with a borderless window spanning the whole screen instead of "
      "using exclusive mode. Allows for faster transitions between fullscreen and windowed mode, "
      "but slightly increases input latency, makes movement less smooth and slightly decreases "
      "performance.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
#endif
  static const char crop_aspect_help[] = QT_TR_NOOP(
      "Crops the picture from its native aspect ratio (which rarely exactly matches 4:3 or 16:9, "
      "because it often includes overscan) to the specific user target aspect ratio (e.g. 4:3 "
      "or 16:9).<br><br>"
      "This option was previously known as Graphics/Advanced/Misc/Crop.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char crop_custom_help[] = QT_TR_NOOP(
      "Enables options to crop the picture by discrete native pixels (independent of the "
      "internal resolution setting) in order to attain a specific user target aspect ratio. "
      "Useful to resolve letterboxing issues. <br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char crop_left_help[] = QT_TR_NOOP(
      "Crops the picture left side by discrete native pixels (independent of the internal "
      "resolution setting) in order to attain a specific user target aspect ratio.  <br><br>"
      "<dolphin_emphasis>If unsure, leave this value to 0.</dolphin_emphasis>");
  static const char crop_top_help[] = QT_TR_NOOP(
      "Crops the picture top side by discrete native pixels (independent of the internal "
      "resolution setting) in order to attain a specific user target aspect ratio.  <br><br>"
      "<dolphin_emphasis>If unsure, leave this value to 0.</dolphin_emphasis>");
  static const char crop_right_help[] = QT_TR_NOOP(
      "Crops the picture right side by discrete native pixels (independent of the internal "
      "resolution setting) in order to attain a specific user target aspect ratio.  <br><br>"
      "<dolphin_emphasis>If unsure, leave this value to 0.</dolphin_emphasis>");
  static const char crop_bottom_help[] = QT_TR_NOOP(
      "Crops the picture bottom side by discrete native pixels (independent of the internal "
      "resolution setting) in order to attain a specific user target aspect ratio.  <br><br>"
      "<dolphin_emphasis>If unsure, leave this value to 0.</dolphin_emphasis>");
  static const char unsure_unchecked[] =
      QT_TR_NOOP("<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");

  m_enable_wireframe->SetDescription(tr(wireframe_help));
  m_enable_format_overlay->SetDescription(tr(format_overlay_help));
  m_enable_api_validation->SetDescription(tr(validation_help));
  m_log_render_time->SetDescription(tr(render_time_help));
  m_dump_textures->SetDescription(tr(dump_textures_help));
  m_dump_mip_textures->SetDescription(tr(dump_mips_help));
  m_dump_base_textures->SetDescription(tr(dump_base_help));
  m_load_custom_textures->SetDescription(tr(load_custom_help));
  m_prefetch_custom_textures->SetDescription(tr(prefetch_help));
  m_dump_efb_target->SetDescription(tr(dump_efb_help));
  m_dump_xfb_target->SetDescription(tr(dump_xfb_help));
  m_disable_vram_copies->SetDescription(tr(vram_copies_help));
  m_enable_graphics_mods->SetDescription(tr(graphics_mods_help));
  m_frame_dumps_resolution_type->SetDescription(tr(resolution_type_help));
#ifdef HAVE_FFMPEG
  m_dump_use_lossless->SetDescription(tr(lossless_help));
#endif
  m_png_compression_level->SetDescription(tr(png_level_help));
  m_enable_prog_scan->SetDescription(tr(prog_scan_help));
  m_backend_multithreading->SetDescription(tr(multithreading_help));

  // Explains why the option may be locked for the current backend.
  const QString backend = tr(g_backend_info.DisplayName.c_str());
  QString vs_note;
  if (!g_backend_info.bSupportsGeometryShaders)
    vs_note = tr("Forced on because %1 doesn't support geometry shaders.").arg(backend);
  else if (!g_backend_info.bSupportsVSLinePointExpand)
    vs_note = tr("Forced off because %1 doesn't support VS expansion.").arg(backend);
  else
    vs_note = tr(unsure_unchecked);
  m_prefer_vs_for_point_line_expansion->SetDescription(tr(vs_expansion_help).arg(vs_note));

  m_cpu_cull->SetDescription(tr(cpu_cull_help));
#ifdef _WIN32
  m_borderless_fullscreen->SetDescription(tr(borderless_help));
#endif
  m_crop_to_aspect_ratio->SetDescription(tr(crop_aspect_help));
  m_crop_custom->SetDescription(tr(crop_custom_help));
  m_crop_custom_left->SetDescription(tr(crop_left_help));
  m_crop_custom_top->SetDescription(tr(crop_top_help));
  m_crop_custom_right->SetDescription(tr(crop_right_help));
  m_crop_custom_bottom->SetDescription(tr(crop_bottom_help));
  m_defer_efb_access_invalidation->SetDescription(tr(defer_invalidation_help));
  m_manual_texture_sampling->SetDescription(tr(manual_sampling_help));
}
