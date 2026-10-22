// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/GraphicsModListWidget.h"

#include <algorithm>
#include <set>

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/Config/GraphicsModWarningWidget.h"
#include "DolphinQt/QtUtils/ClearLayoutRecursively.h"
#include "DolphinQt/Settings.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/VideoConfig.h"

GraphicsModListWidget::GraphicsModListWidget(const UICommon::GameFile& game)
    : m_game_id(game.GetGameID()), m_mod_group(m_game_id)
{
  TrackEmulationState(Core::GetState(Core::System::GetInstance()));
  // Continue the running game's change count so it notices our edits.
  if (m_game_running && g_Config.graphics_mod_config)
    m_mod_group.SetChangeCount(g_Config.graphics_mod_config->GetChangeCount());

  auto* const outer = new QVBoxLayout(this);
  auto* const columns = new QHBoxLayout;
  auto* const left = new QVBoxLayout;

  // Rows are dragged to set the mods' priority.
  m_list = new QListWidget;
  m_list->setSortingEnabled(false);
  m_list->setSelectionBehavior(QAbstractItemView::SelectionBehavior::SelectItems);
  m_list->setSelectionMode(QAbstractItemView::SelectionMode::SingleSelection);
  m_list->setSelectionRectVisible(true);
  m_list->setDragDropMode(QAbstractItemView::InternalMove);

  m_open_folder = new QPushButton(tr("Open Directory..."));
  m_reload = new QPushButton(tr("&Refresh List"));
  auto* const buttons = new QHBoxLayout;
  buttons->addWidget(m_open_folder);
  buttons->addWidget(m_reload);

  left->addWidget(m_list);
  left->addLayout(buttons);

  auto* const right = new QVBoxLayout;
  m_title = new QLabel();
  right->addWidget(m_title);
  m_details = new QVBoxLayout;
  right->addLayout(m_details);
  right->addStretch();

  columns->addLayout(left);
  columns->addLayout(right, 1);

  m_warning = new GraphicsModWarningWidget(this);
  outer->addWidget(m_warning);
  outer->addLayout(columns);
  setLayout(outer);

  connect(m_warning, &GraphicsModWarningWidget::GraphicsModEnableSettings, this,
          &GraphicsModListWidget::OpenGraphicsSettings);
  connect(m_list, &QListWidget::itemSelectionChanged, this,
          &GraphicsModListWidget::OnSelectionChanged);
  connect(m_list, &QListWidget::itemChanged, this, &GraphicsModListWidget::OnCheckToggled);
  connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
          &GraphicsModListWidget::OnRowsMoved);
  connect(m_open_folder, &QPushButton::clicked, this, &GraphicsModListWidget::OpenModFolder);
  connect(m_reload, &QPushButton::clicked, this, &GraphicsModListWidget::Reload);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &GraphicsModListWidget::TrackEmulationState);

  Reload();
  ShowDetails(std::nullopt);
}

GraphicsModListWidget::~GraphicsModListWidget()
{
  if (m_unsaved)
    m_mod_group.Save();
}

void GraphicsModListWidget::Reload()
{
  if (m_unsaved)
    SaveToDisk();

  m_list->setCurrentItem(nullptr);
  m_list->clear();

  m_mod_group = GraphicsModGroupConfig(m_game_id);
  m_mod_group.Load();

  // A mod is listed only if one of its features targets a group that some
  // mod of this game defines.
  std::set<std::string> known_groups;
  for (const GraphicsModConfig& mod : m_mod_group.GetMods())
  {
    for (const GraphicsTargetGroupConfig& group : mod.m_groups)
      known_groups.insert(group.m_name);
  }

  for (const GraphicsModConfig& mod : m_mod_group.GetMods())
  {
    const bool usable =
        std::ranges::any_of(mod.m_features, [&known_groups](const GraphicsModFeatureConfig& f) {
          return known_groups.contains(f.m_group);
        });
    if (!usable)
      continue;

    auto* const row = new QListWidgetItem(QString::fromStdString(mod.m_title));
    row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
    row->setData(Qt::UserRole, QString::fromStdString(mod.GetAbsolutePath()));
    row->setCheckState(mod.m_enabled ? Qt::Checked : Qt::Unchecked);
    m_list->addItem(row);
  }
}

void GraphicsModListWidget::OnSelectionChanged()
{
  QListWidgetItem* const current = m_list->currentItem();
  if (current == nullptr || m_list->count() == 0)
    return;
  ShowDetails(current->data(Qt::UserRole).toString().toStdString());
}

void GraphicsModListWidget::OnCheckToggled(QListWidgetItem* item)
{
  GraphicsModConfig* const mod = m_mod_group.GetMod(item->data(Qt::UserRole).toString().toStdString());
  if (mod == nullptr)
    return;

  const bool enable = item->checkState() == Qt::Checked;
  const bool changed = mod->m_enabled != enable;
  mod->m_enabled = enable;
  if (!changed)
    return;

  m_mod_group.SetChangeCount(m_mod_group.GetChangeCount() + 1);
  PushToRunningGame();
}

void GraphicsModListWidget::OnRowsMoved()
{
  // Each mod's weight becomes its row number.
  QAbstractItemModel* const model = m_list->model();
  for (int row = 0; row < m_list->count(); ++row)
  {
    const std::string path = model->data(model->index(row, 0), Qt::UserRole).toString().toStdString();
    m_mod_group.GetMod(path)->m_weight = static_cast<u16>(row);
  }
  PushToRunningGame();
}

void GraphicsModListWidget::PushToRunningGame()
{
  if (m_game_running)
    g_Config.graphics_mod_config = m_mod_group;
  m_unsaved = true;
}

void GraphicsModListWidget::ShowDetails(const std::optional<std::string>& mod_path)
{
  ClearLayoutRecursively(m_details);
  adjustSize();

  if (!mod_path)
  {
    m_title->setText(tr("No graphics mod selected"));
    m_title->setAlignment(Qt::AlignCenter);
    return;
  }

  const GraphicsModConfig* const mod = m_mod_group.GetMod(*mod_path);
  if (mod == nullptr)
    return;

  m_title->setText(QString::fromStdString(mod->m_title));
  m_title->setAlignment(Qt::AlignLeft);
  QFont heading = m_title->font();
  heading.setWeight(QFont::Bold);
  m_title->setFont(heading);

  if (!mod->m_author.empty())
    m_details->addWidget(new QLabel(tr("By: %1").arg(QString::fromStdString(mod->m_author))));

  if (!mod->m_description.empty())
  {
    auto* const about =
        new QLabel(tr("Description: %1").arg(QString::fromStdString(mod->m_description)));
    about->setWordWrap(true);
    m_details->addWidget(about);
  }
}

void GraphicsModListWidget::SaveToDisk()
{
  m_unsaved = false;
  m_mod_group.Save();
}

const GraphicsModGroupConfig& GraphicsModListWidget::GetGraphicsModConfig() const
{
  return m_mod_group;
}

void GraphicsModListWidget::TrackEmulationState(Core::State state)
{
  // Changes only go live when the game being edited is the one running.
  m_game_running =
      state == Core::State::Running && m_game_id == SConfig::GetInstance().GetGameID();
}

void GraphicsModListWidget::OpenModFolder()
{
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QString::fromStdString(File::GetUserPath(D_GRAPHICSMOD_IDX))));
}
