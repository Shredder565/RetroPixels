// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/GameConfigWidget.h"

#include <optional>
#include <vector>

#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Config/Layer.h"
#include "Common/FileUtil.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigInteger.h"
#include "DolphinQt/Config/ConfigControls/ConfigRadio.h"
#include "DolphinQt/Config/GameConfigEdit.h"
#include "DolphinQt/Config/Graphics/GraphicsPane.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"
#include "DolphinQt/Settings.h"
#include "UICommon/GameFile.h"

namespace
{
// Adds an editor tab for each of the game's INI files found in `directory`.
void AddIniEditors(QTabWidget* tabs, const std::string& directory, const std::string& game_id,
                   u16 revision, bool read_only)
{
  for (const std::string& name : ConfigLoaders::GetGameIniFilenames(game_id, revision))
  {
    const std::string full_path = directory + name;
    if (!File::Exists(full_path))
      continue;
    tabs->addTab(new GameConfigEdit(nullptr, QString::fromStdString(full_path), read_only),
                 QString::fromStdString(name));
  }
}

QString DepthText(const ConfigFloatSlider* slider)
{
  return QString::asprintf("%.0f%%", slider->GetValue());
}

QString ConvergenceText(const ConfigFloatSlider* slider)
{
  return QString::asprintf("%.2f", slider->GetValue());
}

void MakeItalic(QWidget* widget)
{
  QFont font = widget->font();
  font.setItalic(true);
  widget->setFont(font);
}
}  // namespace

GameConfigWidget::GameConfigWidget(const UICommon::GameFile& game) : m_game(game)
{
  m_game_id = m_game.GetGameID();
  m_gameini_local_path =
      QString::fromStdString(File::GetUserPath(D_GAMESETTINGS_IDX) + m_game_id + ".ini");

  m_layer = std::make_unique<Config::Layer>(
      ConfigLoaders::GenerateLocalGameConfigLoader(m_game_id, m_game.GetRevision()));
  m_global_layer = std::make_unique<Config::Layer>(
      ConfigLoaders::GenerateGlobalGameConfigLoader(m_game_id, m_game.GetRevision()));

  QWidget* const general = BuildGeneralTab();
  QWidget* const editors = BuildEditorTab();

  auto* const tabs = new QTabWidget;
  tabs->addTab(general, tr("General"));
  tabs->addTab(new GraphicsPane{nullptr, m_layer.get()}, tr("Graphics"));
  m_editor_tab = tabs->addTab(editors, tr("Editor"));
  connect(tabs, &QTabWidget::currentChanged, this, &GameConfigWidget::OnTabChanged);

  const QString legend =
      tr("Italics mark default game settings, bold marks user settings.\nRight-click to remove "
         "user settings.\nGraphics tabs don't display the value of a default game "
         "setting.\nAnti-Aliasing settings are disabled when the global graphics backend "
         "doesn't match the game setting.");
  auto* const notice = new QLabel(tr("These settings override core Dolphin settings."));
  QWidget* const notice_row =
      QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxQuestion, notice);
  notice_row->setToolTip(legend);

  auto* const column = new QVBoxLayout;
  column->addWidget(notice_row);
  column->addWidget(tabs);
  setLayout(column);

  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          &GameConfigWidget::ShowGlobalValues);

  AddIniEditors(m_default_inis, File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP, m_game_id,
                m_game.GetRevision(), true);
  AddIniEditors(m_user_inis, File::GetUserPath(D_GAMESETTINGS_IDX), m_game_id,
                m_game.GetRevision(), false);

  // The user INI named after the game ID always gets an editor, even when
  // the file doesn't exist yet.
  bool has_own_ini = false;
  for (int i = 0; i < m_user_inis->count(); ++i)
  {
    if (m_user_inis->tabText(i).toStdString() == m_game_id + ".ini")
      has_own_ini = true;
  }
  if (!has_own_ini)
  {
    m_user_inis->addTab(
        new GameConfigEdit(
            nullptr,
            QString::fromStdString(File::GetUserPath(D_GAMESETTINGS_IDX) + m_game_id + ".ini"),
            false),
        UserIniName());
  }

  // Fonts set during construction don't stick, so the italics and the first
  // refresh wait a moment.
  QTimer::singleShot(100, this, [this] {
    MarkGlobalSettings();
    Config::OnConfigChanged();
  });
}

GameConfigWidget::~GameConfigWidget()
{
  // Destroying the layer writes it to the user INI.
  m_layer.reset();

  // If this game is running, reload its local game layer so the changes
  // apply now.
  const std::shared_ptr<Config::Layer> running = Config::GetLayer(Config::LayerType::LocalGame);
  if (running && SConfig::GetInstance().GetGameID() == m_game_id)
  {
    running->DeleteAllKeys();
    running->Load();
    Config::OnConfigChanged();
  }

  // Don't leave an empty INI behind.
  if (File::GetSize(m_gameini_local_path.toStdString()) == 0)
    File::Delete(m_gameini_local_path.toStdString());
}

QString GameConfigWidget::UserIniName() const
{
  return QString::fromStdString(m_game_id + ".ini");
}

QWidget* GameConfigWidget::BuildGeneralTab()
{
  Config::Layer* const layer = m_layer.get();

  auto* const core_group = new QGroupBox(tr("Core"));
  auto* const core_grid = new QGridLayout;
  core_group->setLayout(core_grid);

  m_dual_core = new ConfigBool(tr("Enable Dual Core"), Config::MAIN_CPU_THREAD, layer);
  m_mmu = new ConfigBool(tr("Enable MMU"), Config::MAIN_MMU, layer);
  m_fprf = new ConfigBool(tr("Enable FPRF"), Config::MAIN_FPRF, layer);
  m_sync_gpu = new ConfigBool(tr("Synchronize GPU thread"), Config::MAIN_SYNC_GPU, layer);
  // Stored as "fast disc speed", so the box is inverted.
  m_disc_speed = new ConfigBool(tr("Emulate Disc Speed"), Config::MAIN_FAST_DISC_SPEED, layer, true);
  m_dsp_hle = new ConfigBool(tr("DSP HLE (fast)"), Config::MAIN_DSP_HLE, layer);

  // The translated names are also the stored values.
  const std::vector<std::string> modes{tr("auto").toStdString(), tr("none").toStdString(),
                                       tr("fake-completion").toStdString()};
  m_determinism = new ConfigStringChoice(modes, Config::MAIN_GPU_DETERMINISM_MODE, layer);

  m_mmu->SetDescription(tr("Enables the Memory Management Unit, needed for some games. "
                           "(ON = Compatible, OFF = Fast)"));
  m_fprf->SetDescription(tr("Enables Floating Point Result Flag calculation, needed for a "
                            "few games. (ON = Compatible, OFF = Fast)"));
  m_sync_gpu->SetDescription(tr("Synchronizes the GPU and CPU threads to help prevent random "
                                "freezes in Dual core mode. (ON = Compatible, OFF = Fast)"));
  m_disc_speed->SetDescription(tr("Enable emulated disc speed. Disabling this can cause crashes "
                                  "and other problems in some games. (ON = Compatible, OFF = "
                                  "Unlocked)"));

  int row = 0;
  for (QWidget* const box : {static_cast<QWidget*>(m_dual_core), static_cast<QWidget*>(m_mmu),
                             static_cast<QWidget*>(m_fprf), static_cast<QWidget*>(m_sync_gpu),
                             static_cast<QWidget*>(m_disc_speed), static_cast<QWidget*>(m_dsp_hle)})
  {
    core_grid->addWidget(box, row++, 0);
  }
  core_grid->addWidget(new QLabel(tr("Deterministic dual core:")), row, 0);
  core_grid->addWidget(m_determinism, row, 1);

  auto* const stereo_group = new QGroupBox(tr("Stereoscopy"));
  auto* const stereo_grid = new QGridLayout;
  stereo_group->setLayout(stereo_grid);

  m_depth = new ConfigFloatSlider(100, 200, Config::GFX_STEREO_DEPTH_PERCENTAGE, 1.0f, layer);
  m_convergence = new ConfigFloatSlider(0, 1000, Config::GFX_STEREO_CONVERGENCE, 0.01f, layer);
  auto* const depth_value = new QLabel();
  auto* const convergence_value = new QLabel();
  m_mono_shadows =
      new ConfigBool(tr("Monoscopic Shadows"), Config::GFX_STEREO_EFB_MONO_DEPTH, layer);

  m_depth->SetDescription(
      tr("This value is multiplied with the depth set in the graphics configuration."));
  m_convergence->SetDescription(
      tr("This value is added to the convergence value set in the graphics configuration."));
  m_mono_shadows->SetDescription(
      tr("Use a single depth buffer for both eyes. Needed for a few games."));

  stereo_grid->addWidget(new ConfigFloatLabel(tr("Depth Percentage:"), m_depth), 0, 0);
  stereo_grid->addWidget(m_depth, 0, 1);
  stereo_grid->addWidget(depth_value, 0, 2);
  stereo_grid->addWidget(new ConfigFloatLabel(tr("Convergence:"), m_convergence), 1, 0);
  stereo_grid->addWidget(m_convergence, 1, 1);
  stereo_grid->addWidget(convergence_value, 1, 2);
  stereo_grid->addWidget(m_mono_shadows, 2, 0);

  depth_value->setText(DepthText(m_depth));
  convergence_value->setText(ConvergenceText(m_convergence));
  connect(m_depth, &ConfigFloatSlider::valueChanged, this,
          [this, depth_value] { depth_value->setText(DepthText(m_depth)); });
  connect(m_convergence, &ConfigFloatSlider::valueChanged, this,
          [this, convergence_value] { convergence_value->setText(ConvergenceText(m_convergence)); });

  auto* const column = new QVBoxLayout;
  column->addWidget(core_group);
  column->addWidget(stereo_group);
  column->addStretch();

  auto* const page = new QWidget;
  page->setLayout(column);
  return page;
}

QWidget* GameConfigWidget::BuildEditorTab()
{
  auto* const default_group = new QGroupBox(tr("Default Config (Read Only)"));
  auto* const default_column = new QVBoxLayout;
  m_default_inis = new QTabWidget;
  default_group->setLayout(default_column);
  default_column->addWidget(m_default_inis);

  auto* const user_group = new QGroupBox(tr("User Config"));
  auto* const user_column = new QVBoxLayout;
  m_user_inis = new QTabWidget;
  user_group->setLayout(user_column);
  user_column->addWidget(m_user_inis);

  auto* const column = new QVBoxLayout;
  column->addWidget(default_group);
  column->addWidget(user_group);

  auto* const page = new QWidget;
  page->setLayout(column);
  return page;
}

void GameConfigWidget::OnTabChanged(int index)
{
  // Entering the editor: write the layer out and reopen the game's own user
  // INI so the editor shows the latest values.
  if (index == m_editor_tab)
  {
    m_layer->Save();
    ReplaceUserIniEditor();
  }

  // Leaving the editor: reload the layer from the edited files. Load()
  // keeps keys that were deleted, so the layer is emptied first.
  if (m_last_tab == m_editor_tab)
  {
    m_layer->DeleteAllKeys();
    m_layer->Load();
    Config::OnConfigChanged();
  }

  m_last_tab = index;
}

void GameConfigWidget::ReplaceUserIniEditor()
{
  // Other INIs for the game (by game ID prefix, by revision) are untouched.
  for (int i = 0; i < m_user_inis->count(); ++i)
  {
    if (m_user_inis->tabText(i).toStdString() != m_game_id + ".ini")
      continue;

    m_user_inis->removeTab(i);
    auto* const editor = new GameConfigEdit(
        nullptr, QString::fromStdString(File::GetUserPath(D_GAMESETTINGS_IDX) + m_game_id + ".ini"),
        false);
    m_user_inis->insertTab(i, editor, UserIniName());
    return;
  }
}

void GameConfigWidget::ShowGlobalValues()
{
  // A control whose key is only set in the default INIs shows that value.
  // User values are left alone.
  const auto only_global = [this](const Config::Location& location) {
    return !m_layer->Exists(location) && m_global_layer->Exists(location);
  };

  const auto show_bool = [&](ConfigBool* box, bool inverted) {
    const Config::Location& location = box->GetLocation();
    if (!only_global(location))
      return;
    const std::optional<bool> value = m_global_layer->Get<bool>(box->GetLocation());
    if (!value)
      return;
    const QSignalBlocker quiet(box);
    box->setChecked(*value != inverted);
  };

  const auto show_int = [&](ConfigFloatSlider* slider) {
    const Config::Location& location = slider->GetLocation();
    if (!only_global(location))
      return;
    const std::optional<int> value = m_global_layer->Get<int>(location);
    if (!value)
      return;
    const QSignalBlocker quiet(slider);
    slider->setValue(*value);
  };

  for (ConfigBool* const box :
       {m_dual_core, m_mmu, m_fprf, m_sync_gpu, m_dsp_hle, m_mono_shadows})
  {
    show_bool(box, false);
  }
  show_bool(m_disc_speed, true);

  show_int(m_depth);
  show_int(m_convergence);
}

void GameConfigWidget::MarkGlobalSettings()
{
  // Settings that have a default game value are drawn in italics. This only
  // runs once; the default INIs don't change while the dialog is open.
  const auto mark = [this](auto* control) {
    if (m_global_layer->Exists(control->GetLocation()))
      MakeItalic(control);
  };

  for (ConfigBool* const control : findChildren<ConfigBool*>())
    mark(control);
  for (ConfigFloatSlider* const control : findChildren<ConfigFloatSlider*>())
    mark(control);
  for (ConfigInteger* const control : findChildren<ConfigInteger*>())
    mark(control);
  for (ConfigRadioInt* const control : findChildren<ConfigRadioInt*>())
    mark(control);
  for (ConfigChoice* const control : findChildren<ConfigChoice*>())
    mark(control);
  for (ConfigStringChoice* const control : findChildren<ConfigStringChoice*>())
    mark(control);

  // Complex choices span two keys; either one counts.
  for (ConfigComplexChoice* const control : findChildren<ConfigComplexChoice*>())
  {
    const auto [first, second] = control->GetLocation();
    if (m_global_layer->Exists(first) || m_global_layer->Exists(second))
      MakeItalic(control);
  }
}
