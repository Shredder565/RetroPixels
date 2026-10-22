// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Graphics/GeneralWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigInteger.h"
#include "DolphinQt/Config/ConfigControls/ConfigRadio.h"
#include "DolphinQt/Config/GameConfigWidget.h"
#include "DolphinQt/Config/Graphics/GraphicsPane.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipComboBox.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Settings.h"

#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// Largest width or height accepted for a custom aspect ratio.
constexpr int CUSTOM_ASPECT_LIMIT = 10000;

QString CurrentBackendName()
{
  return QString::fromStdString(Config::Get(Config::MAIN_GFX_BACKEND));
}
}  // namespace

GeneralWidget::GeneralWidget(GraphicsPane* gfx_pane) : m_game_layer{gfx_pane->GetConfigLayer()}
{
  auto* const column = new QVBoxLayout;
  QGroupBox* const basic = BuildBasicGroup();
  QGroupBox* const other = BuildOtherGroup();
  QGroupBox* const shaders = BuildShaderGroup();
  column->addWidget(basic);
  column->addWidget(other);
  column->addWidget(shaders);
  column->addStretch();
  setLayout(column);

  connect(m_backend_combo, &QComboBox::currentIndexChanged, this,
          &GeneralWidget::ConfirmBackendChoice);
  connect(m_adapter_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
    Config::SetBaseOrCurrent(Config::GFX_ADAPTER, index);
    emit BackendChanged(CurrentBackendName());
  });
  connect(m_aspect_combo, &QComboBox::currentIndexChanged, this,
          &GeneralWidget::ShowCustomAspectFields);

  SetDescriptions();

  connect(gfx_pane, &GraphicsPane::BackendChanged, this, &GeneralWidget::FillAdapterList);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    OnEmulationStateChanged(state != Core::State::Uninitialized);
  });
  OnEmulationStateChanged(!Core::IsUninitialized(Core::System::GetInstance()));
}

QGroupBox* GeneralWidget::BuildBasicGroup()
{
  auto* const box = new QGroupBox(tr("Basic"));
  auto* const grid = new QGridLayout{box};

  // Item text is the translated display name, item data the INI name.
  std::vector<std::pair<QString, QString>> backends;
  for (const auto& backend : VideoBackendBase::GetAvailableBackends())
  {
    backends.emplace_back(tr(backend->GetDisplayName().c_str()),
                          QString::fromStdString(backend->GetConfigName()));
  }
  m_backend_combo = new ConfigStringChoice(backends, Config::MAIN_GFX_BACKEND, m_game_layer);
  m_last_backend_index = m_backend_combo->currentIndex();

  m_aspect_combo = new ConfigChoice({tr("Auto"), tr("Force 16:9"), tr("Force 4:3"),
                                     tr("Stretch to Window"), tr("Custom"), tr("Custom (Stretch)")},
                                    Config::GFX_ASPECT_RATIO, m_game_layer);
  m_custom_aspect_label = new QLabel(tr("Custom Aspect Ratio:"));
  m_custom_aspect_width = new ConfigInteger(1, CUSTOM_ASPECT_LIMIT,
                                            Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH, m_game_layer);
  m_custom_aspect_height = new ConfigInteger(1, CUSTOM_ASPECT_LIMIT,
                                             Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT, m_game_layer);
  ShowCustomAspectFields(m_aspect_combo->currentIndex());

  m_adapter_combo = new ToolTipComboBox;
  m_enable_vsync = new ConfigBool(tr("V-Sync"), Config::GFX_VSYNC, m_game_layer);
  m_enable_fullscreen =
      new ConfigBool(tr("Start in Fullscreen"), Config::MAIN_FULLSCREEN, m_game_layer);

  // Captioned rows; the control spans the rest of the row.
  const std::pair<QString, QWidget*> rows[] = {
      {tr("Backend:"), m_backend_combo},
      {tr("Adapter:"), m_adapter_combo},
      {tr("Aspect Ratio:"), m_aspect_combo},
  };
  int row = 0;
  for (const auto& [caption, control] : rows)
  {
    grid->addWidget(new QLabel(caption), row, 0);
    grid->addWidget(control, row, 1, 1, -1);
    ++row;
  }
  grid->addWidget(m_custom_aspect_label, row, 0);
  grid->addWidget(m_custom_aspect_width, row, 1);
  grid->addWidget(m_custom_aspect_height, row, 2);

  auto* const toggles = new QGridLayout;
  grid->addLayout(toggles, grid->rowCount(), 0, 1, -1);
  toggles->addWidget(m_enable_vsync, 0, 0);
  toggles->addWidget(m_enable_fullscreen, 0, 1);

  // Global only: not offered per game.
  auto* const precise_timing =
      new ConfigBool(tr("Precision Frame Timing"), Config::MAIN_PRECISION_FRAME_TIMING);
  precise_timing->SetDescription(
      tr("Uses high resolution timers and \"busy waiting\" for improved frame pacing.<br><br>"
         "This will marginally increase power usage.<br><br>"
         "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));
  toggles->addWidget(precise_timing, 1, 0);

  return box;
}

QGroupBox* GeneralWidget::BuildOtherGroup()
{
  auto* const box = new QGroupBox(tr("Other"));
  auto* const grid = new QGridLayout();

  m_autoadjust_window_size = new ConfigBool(tr("Auto-Adjust Window Size"),
                                            Config::MAIN_RENDER_WINDOW_AUTOSIZE, m_game_layer);
  m_render_main_window =
      new ConfigBool(tr("Render to Main Window"), Config::MAIN_RENDER_TO_MAIN, m_game_layer);

  box->setLayout(grid);
  grid->addWidget(m_render_main_window, 0, 0);
  grid->addWidget(m_autoadjust_window_size, 0, 1);
  return box;
}

QGroupBox* GeneralWidget::BuildShaderGroup()
{
  auto* const box = new QGroupBox(tr("Shader Compilation"));
  auto* const grid = new QGridLayout();

  // Radio value i is ShaderCompilationMode i; two per row.
  static constexpr std::array<const char*, 4> mode_names = {
      QT_TR_NOOP("Specialized (Default)"),
      QT_TR_NOOP("Exclusive Ubershaders"),
      QT_TR_NOOP("Hybrid Ubershaders"),
      QT_TR_NOOP("Skip Drawing"),
  };
  for (int mode = 0; mode < static_cast<int>(mode_names.size()); ++mode)
  {
    auto* const radio =
        new ConfigRadioInt(tr(mode_names[static_cast<size_t>(mode)]),
                           Config::GFX_SHADER_COMPILATION_MODE, mode, m_game_layer);
    m_shader_modes[static_cast<size_t>(mode)] = radio;
    grid->addWidget(radio, mode / 2, mode % 2);
  }

  m_wait_for_shaders = new ConfigBool(tr("Compile Shaders Before Starting"),
                                      Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, m_game_layer);
  grid->addWidget(m_wait_for_shaders);
  box->setLayout(grid);
  return box;
}

void GeneralWidget::SetDescriptions()
{
  static const char backend_help[] = QT_TR_NOOP(
      "Selects which graphics API to use internally.<br><br>"
      "The software renderer is extremely slow and only useful for debugging, so any of the other "
      "backends are recommended. Different games and different GPUs will behave differently on "
      "each backend, so for the best emulation experience it is recommended to try each and select "
      "the backend that is least problematic.<br><br>"
      "<dolphin_emphasis>If unsure, select %1.</dolphin_emphasis>");
  static const char fullscreen_help[] = QT_TR_NOOP(
      "Uses the entire screen for rendering.<br><br>"
      "If disabled, a render window will be created instead.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char autosize_help[] = QT_TR_NOOP(
      "Automatically adjusts the window size to the internal resolution.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char main_window_help[] = QT_TR_NOOP(
      "Uses the main Dolphin window for rendering rather than a separate render window.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char aspect_help[] = QT_TR_NOOP(
      "Selects which aspect ratio to use for displaying the game.<br><br>"
      "The aspect ratio of the image sent out by the original consoles varied depending on the "
      "game and rarely exactly matched 4:3 or 16:9. Some of the image would be cut off by the edges "
      "of the TV, or the image wouldn't fill the TV entirely. By default, Dolphin shows the whole "
      "image without distorting its proportions, which means it's normal for the image to not "
      "entirely fill your display.<br><br>"
      "<b>Auto</b>: Mimics a TV with either a 4:3 or 16:9 aspect ratio, depending on which type "
      "of TV the game seems to be targeting.<br><br>"
      "<b>Force 16:9</b>: Mimics a TV with a 16:9 (widescreen) aspect ratio.<br><br>"
      "<b>Force 4:3</b>: Mimics a TV with a 4:3 aspect ratio.<br><br>"
      "<b>Stretch to Window</b>: Stretches the image to the window size. This will usually "
      "distort the image's proportions.<br><br>"
      "<b>Custom</b>: Mimics a TV with the specified aspect ratio. This is mostly intended to be "
      "used with aspect ratio cheats/mods.<br><br>"
      "<b>Custom (Stretch)</b>: Similar to `Custom`, but stretches the image to the specified "
      "aspect ratio. This will usually distort the image's proportions, and should not be used "
      "under normal circumstances.<br><br>"
      "<dolphin_emphasis>If unsure, select Auto.</dolphin_emphasis>");
  static const char vsync_help[] = QT_TR_NOOP(
      "Waits for vertical blanks in order to prevent tearing.<br><br>"
      "Decreases performance if emulation speed is below 100%.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");
  static const char specialized_help[] = QT_TR_NOOP(
      "Ubershaders are never used. Stuttering will occur during shader compilation, but GPU "
      "demands are low.<br><br>"
      "Recommended for low-end hardware. <br><br>"
      "<dolphin_emphasis>If unsure, select this mode.</dolphin_emphasis>");
  // "Very powerful" is meant by the PC graphics cards of around 2021.
  static const char exclusive_uber_help[] = QT_TR_NOOP(
      "Ubershaders will always be used. Provides a near stutter-free experience at the cost of very "
      "high GPU performance requirements.<br><br>"
      "<dolphin_emphasis>Don't use this unless you encountered stuttering with Hybrid Ubershaders "
      "and have a very powerful GPU.</dolphin_emphasis>");
  static const char hybrid_uber_help[] = QT_TR_NOOP(
      "Ubershaders will be used to prevent stuttering during shader compilation, but specialized "
      "shaders will be used when they will not cause stuttering.<br><br>"
      "In the best case it eliminates shader compilation stuttering while having minimal "
      "performance impact, but results depend on video driver behavior.");
  static const char skip_drawing_help[] = QT_TR_NOOP(
      "Prevents shader compilation stuttering by not rendering waiting objects. Can work in "
      "scenarios where Ubershaders doesn't, at the cost of introducing visual glitches and broken "
      "effects.<br><br>"
      "<dolphin_emphasis>Not recommended, only use if the other options give poor "
      "results.</dolphin_emphasis>");
  static const char wait_for_shaders_help[] = QT_TR_NOOP(
      "Waits for all shaders to finish compiling before starting a game. Enabling this option may "
      "reduce stuttering or hitching for a short time after the game is started, at the cost of a "
      "longer delay before the game starts. For systems with two or fewer cores, it is recommended "
      "to enable this option, as a large shader queue may reduce frame rates.<br><br>"
      "<dolphin_emphasis>Otherwise, if unsure, leave this unchecked.</dolphin_emphasis>");

  m_backend_combo->SetTitle(tr("Backend"));
  m_backend_combo->SetDescription(tr(backend_help).arg(
      QString::fromStdString(VideoBackendBase::GetDefaultBackendDisplayName())));
  m_adapter_combo->SetTitle(tr("Adapter"));
  m_aspect_combo->SetTitle(tr("Aspect Ratio"));
  m_aspect_combo->SetDescription(tr(aspect_help));
  m_custom_aspect_width->SetTitle(tr("Custom Aspect Ratio Width"));
  m_custom_aspect_height->SetTitle(tr("Custom Aspect Ratio Height"));
  m_enable_vsync->SetDescription(tr(vsync_help));
  m_enable_fullscreen->SetDescription(tr(fullscreen_help));
  m_autoadjust_window_size->SetDescription(tr(autosize_help));
  m_render_main_window->SetDescription(tr(main_window_help));

  const char* const mode_help[] = {specialized_help, exclusive_uber_help, hybrid_uber_help,
                                   skip_drawing_help};
  for (size_t i = 0; i < m_shader_modes.size(); ++i)
    m_shader_modes[i]->SetDescription(tr(mode_help[i]));
  m_wait_for_shaders->SetDescription(tr(wait_for_shaders_help));
}

void GeneralWidget::ShowCustomAspectFields(int aspect_index)
{
  const bool custom = aspect_index == static_cast<int>(AspectMode::Custom) ||
                      aspect_index == static_cast<int>(AspectMode::CustomStretch);
  for (QWidget* const field : {static_cast<QWidget*>(m_custom_aspect_label),
                               static_cast<QWidget*>(m_custom_aspect_width),
                               static_cast<QWidget*>(m_custom_aspect_height)})
  {
    field->setHidden(!custom);
  }
}

// Some backends carry a warning (the software renderer, for one). Changing
// the global backend to one of those asks first, and a "No" puts the old
// choice back. Game-specific or overridden backends are not asked about.
void GeneralWidget::ConfirmBackendChoice()
{
  if (Config::GetActiveLayerForConfig(Config::MAIN_GFX_BACKEND) == Config::LayerType::Base)
  {
    const auto& backends = VideoBackendBase::GetAvailableBackends();
    if (backends.empty())
      return;

    const int picked = m_backend_combo->currentIndex();
    if (picked == -1)  // the setting names no available backend
      return;

    const auto warning = backends[static_cast<size_t>(picked)]->GetWarningMessage();
    if (warning)
    {
      ModalMessageBox question(this);
      question.setIcon(QMessageBox::Warning);
      question.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
      question.setWindowTitle(tr("Confirm backend change"));
      question.setText(tr(warning->c_str()));
      if (question.exec() != QMessageBox::Yes)
      {
        m_backend_combo->setCurrentIndex(m_last_backend_index);
        return;
      }
    }
  }

  m_last_backend_index = m_backend_combo->currentIndex();
  emit BackendChanged(m_backend_combo->currentData().toString());
}

void GeneralWidget::OnEmulationStateChanged(bool running)
{
  // These cannot change under a running game.
  for (QWidget* const option : {static_cast<QWidget*>(m_backend_combo),
                                static_cast<QWidget*>(m_render_main_window),
                                static_cast<QWidget*>(m_enable_fullscreen)})
  {
    option->setEnabled(!running);
  }
  m_adapter_combo->setEnabled(!running && !g_backend_info.Adapters.empty());

  // Something else may have changed the backend (a game's INI, NetPlay);
  // reselect it quietly and let the other tabs know.
  const std::string shown = m_backend_combo->currentData().toString().toStdString();
  if (Config::Get(Config::MAIN_GFX_BACKEND) == shown)
    return;
  {
    const QSignalBlocker quiet(m_backend_combo);
    m_backend_combo->Load();
  }
  emit BackendChanged(CurrentBackendName());
}

void GeneralWidget::FillAdapterList(const QString&)
{
  const QSignalBlocker quiet(m_adapter_combo);

  const std::vector<std::string>& adapters = g_backend_info.Adapters;
  m_adapter_combo->clear();
  for (const std::string& adapter : adapters)
    m_adapter_combo->addItem(QString::fromStdString(adapter));

  const int saved = Config::Get(Config::GFX_ADAPTER);
  if (saved < m_adapter_combo->count())
    m_adapter_combo->setCurrentIndex(saved);

  const bool has_adapters = !adapters.empty();
  m_adapter_combo->setEnabled(has_adapters && Core::IsUninitialized(Core::System::GetInstance()));

  static constexpr char with_adapters[] =
      QT_TR_NOOP("Selects a hardware adapter to use.<br><br>"
                 "<dolphin_emphasis>If unsure, select the first one.</dolphin_emphasis>");
  static constexpr char without_adapters[] =
      QT_TR_NOOP("Selects a hardware adapter to use.<br><br>"
                 "<dolphin_emphasis>%1 doesn't support this feature.</dolphin_emphasis>");
  if (has_adapters)
  {
    m_adapter_combo->SetDescription(tr(with_adapters));
  }
  else
  {
    m_adapter_combo->SetDescription(
        tr(without_adapters).arg(tr(g_video_backend->GetDisplayName().c_str())));
  }
}
