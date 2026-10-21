// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/PatchesWidget.h"

#include <QGridLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/ConfigManager.h"
#include "Core/PatchEngine.h"
#include "DolphinQt/Config/HardcoreWarningWidget.h"
#include "DolphinQt/Config/NewPatchDialog.h"
#include "UICommon/GameFile.h"

namespace
{
std::string UserIniPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
}
}  // namespace

PatchesWidget::PatchesWidget(const UICommon::GameFile& game)
    : m_game_id(game.GetGameID()), m_game_revision(game.GetRevision())
{
  {
    Common::IniFile user_ini;
    user_ini.Load(UserIniPath(m_game_id));
    const Common::IniFile default_ini =
        SConfig::GetInstance().LoadDefaultGameIni(m_game_id, m_game_revision);
    PatchEngine::LoadPatchSection("OnFrame", &m_patches, default_ini, user_ini);
  }

#ifdef USE_RETRO_ACHIEVEMENTS
  m_hardcore_warning = new HardcoreWarningWidget(this);
#endif  // USE_RETRO_ACHIEVEMENTS
  m_list = new QListWidget;
  m_add_button = new QPushButton(tr("&Add..."));
  m_edit_button = new QPushButton();
  m_remove_button = new QPushButton(tr("&Remove"));

  // The list spans the row of buttons: Add, Remove, then Edit/Clone.
  auto* const grid = new QGridLayout;
  grid->addWidget(m_list, 0, 0, 1, -1);
  grid->addWidget(m_add_button, 1, 0);
  grid->addWidget(m_edit_button, 1, 2);
  grid->addWidget(m_remove_button, 1, 1);

  auto* const column = new QVBoxLayout;
#ifdef USE_RETRO_ACHIEVEMENTS
  column->addWidget(m_hardcore_warning);
#endif  // USE_RETRO_ACHIEVEMENTS
  column->addLayout(grid);
  setLayout(column);

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_hardcore_warning, &HardcoreWarningWidget::OpenAchievementSettings, this,
          &PatchesWidget::OpenAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  connect(m_list, &QListWidget::itemSelectionChanged, this, &PatchesWidget::RefreshButtons);
  connect(m_list, &QListWidget::itemChanged, this, &PatchesWidget::OnCheckToggled);
  connect(m_remove_button, &QPushButton::clicked, this, &PatchesWidget::RemoveSelected);
  connect(m_add_button, &QPushButton::clicked, this, &PatchesWidget::AddPatch);
  connect(m_edit_button, &QPushButton::clicked, this, &PatchesWidget::EditSelected);

  RebuildList();
  RefreshButtons();
}

void PatchesWidget::RebuildList()
{
  m_list->clear();
  for (const PatchEngine::Patch& patch : m_patches)
  {
    auto* const item = new QListWidgetItem(QString::fromStdString(patch.name));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(patch.enabled ? Qt::Checked : Qt::Unchecked);
    // Built-in patches are cloned rather than edited.
    item->setData(Qt::UserRole, patch.user_defined);
    m_list->addItem(item);
  }
}

void PatchesWidget::RefreshButtons()
{
  const QList<QListWidgetItem*> selection = m_list->selectedItems();
  const bool any = !selection.isEmpty();
  // With nothing selected the buttons read as for a user patch.
  const bool own = !any || selection[0]->data(Qt::UserRole).toBool();

  m_edit_button->setEnabled(any);
  m_edit_button->setText(own ? tr("&Edit...") : tr("&Clone..."));
  m_remove_button->setEnabled(any && own);
}

void PatchesWidget::WritePatches()
{
  const std::string path = UserIniPath(m_game_id);
  Common::IniFile user_ini;
  user_ini.Load(path);
  PatchEngine::SavePatchSection(&user_ini, m_patches);
  user_ini.Save(path);
}

bool PatchesWidget::RunEditor(PatchEngine::Patch& patch)
{
  NewPatchDialog editor(this, patch);
  return editor.exec() != 0;
}

void PatchesWidget::OnCheckToggled(QListWidgetItem* item)
{
  m_patches[static_cast<size_t>(m_list->row(item))].enabled = item->checkState() == Qt::Checked;
  WritePatches();
}

void PatchesWidget::AddPatch()
{
  PatchEngine::Patch patch;
  patch.user_defined = true;
  if (!RunEditor(patch))
    return;

  m_patches.push_back(patch);
  WritePatches();
  RebuildList();
}

void PatchesWidget::EditSelected()
{
  if (m_list->selectedItems().isEmpty())
    return;

  QListWidgetItem* const item = m_list->selectedItems()[0];
  PatchEngine::Patch patch = m_patches[static_cast<size_t>(m_list->row(item))];

  if (!patch.user_defined)
  {
    // i18n: Name given to the user's copy of the built-in patch called %1
    // when that patch is edited.
    patch.name = tr("%1 (Copy)").arg(QString::fromStdString(patch.name)).toStdString();
  }

  if (!RunEditor(patch))
    return;

  if (patch.user_defined)
  {
    m_patches[static_cast<size_t>(m_list->row(item))] = patch;
  }
  else
  {
    // The copy is added as a new user patch; the original stays as it was.
    patch.user_defined = true;
    m_patches.push_back(patch);
  }

  WritePatches();
  RebuildList();
}

void PatchesWidget::RemoveSelected()
{
  if (m_list->selectedItems().isEmpty())
    return;

  m_patches.erase(m_patches.begin() + m_list->row(m_list->selectedItems()[0]));
  WritePatches();
  RebuildList();
}
