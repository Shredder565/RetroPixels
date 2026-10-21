// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ARCodeWidget.h"

#include <algorithm>
#include <functional>
#include <utility>

#include <QCursor>
#include <QHBoxLayout>
#ifdef USE_RETRO_ACHIEVEMENTS
#include <QIcon>
#include <QStyle>
#endif  // USE_RETRO_ACHIEVEMENTS
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/AchievementManager.h"
#include "Core/ActionReplay.h"
#include "Core/ConfigManager.h"
#include "DolphinQt/Config/CheatCodeEditor.h"
#include "DolphinQt/Config/CheatWarningWidget.h"
#include "DolphinQt/Config/HardcoreWarningWidget.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Settings.h"
#endif  // USE_RETRO_ACHIEVEMENTS

namespace
{
// User codes always live in GameSettings/<id>.ini in the user directory.
std::string UserIniPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
}

// Names are stored with < and > escaped.
QString DisplayName(const std::string& name)
{
  return QString::fromStdString(name)
      .replace(QStringLiteral("&lt;"), QChar::fromLatin1('<'))
      .replace(QStringLiteral("&gt;"), QChar::fromLatin1('>'));
}
}  // namespace

ARCodeWidget::ARCodeWidget(std::string game_id, u16 game_revision, bool restart_required)
    : m_game_id(std::move(game_id)), m_game_revision(game_revision),
      m_restart_required(restart_required)
{
  BuildLayout();
  WireUp();
  ReadCodes();
}

ARCodeWidget::~ARCodeWidget() = default;

void ARCodeWidget::ChangeGame(std::string game_id, const u16 game_revision)
{
  m_game_id = std::move(game_id);
  m_game_revision = game_revision;
  m_restart_required = false;
  m_codes.clear();

  // An open editor belongs to the previous game's list; close it rather than
  // let it save into the new one.
  m_editor->reject();

  ReadCodes();
}

void ARCodeWidget::BuildLayout()
{
  m_cheat_warning = new CheatWarningWidget(m_game_id, m_restart_required, this);
#ifdef USE_RETRO_ACHIEVEMENTS
  m_hardcore_warning = new HardcoreWarningWidget(this);
#endif  // USE_RETRO_ACHIEVEMENTS
  m_list = new QListWidget;
  m_add_button = new NonDefaultQPushButton(tr("&Add New Code..."));
  m_edit_button = new NonDefaultQPushButton(tr("&Edit Code..."));
  m_remove_button = new NonDefaultQPushButton(tr("&Remove Code"));
  m_toggle_all_button = new NonDefaultQPushButton(tr("Disable All"));
  m_editor = new CheatCodeEditor(this);

  m_list->setContextMenuPolicy(Qt::CustomContextMenu);

  auto* const buttons = new QHBoxLayout;
  for (QPushButton* const button :
       {m_add_button, m_edit_button, m_remove_button, m_toggle_all_button})
  {
    buttons->addWidget(button);
  }

  auto* const column = new QVBoxLayout{this};
  column->addWidget(m_cheat_warning);
#ifdef USE_RETRO_ACHIEVEMENTS
  column->addWidget(m_hardcore_warning);
#endif  // USE_RETRO_ACHIEVEMENTS
  column->addWidget(m_list);
  column->addLayout(buttons);
}

void ARCodeWidget::WireUp()
{
  connect(m_cheat_warning, &CheatWarningWidget::OpenCheatEnableSettings, this,
          &ARCodeWidget::OpenGeneralSettings);
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_hardcore_warning, &HardcoreWarningWidget::OpenAchievementSettings, this,
          &ARCodeWidget::OpenAchievementSettings);
  // The approval icons depend on hardcore mode, which can change with the
  // emulation state.
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &ARCodeWidget::RebuildList);
#endif  // USE_RETRO_ACHIEVEMENTS

  connect(m_list, &QListWidget::itemChanged, this, &ARCodeWidget::OnCheckToggled);
  connect(m_list, &QListWidget::itemSelectionChanged, this, &ARCodeWidget::OnSelectionChanged);
  connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, &ARCodeWidget::OnRowsMoved);
  connect(m_list, &QListWidget::customContextMenuRequested, this, &ARCodeWidget::ShowSortMenu);

  connect(m_add_button, &QPushButton::clicked, this, &ARCodeWidget::NewCode);
  connect(m_edit_button, &QPushButton::clicked, this, &ARCodeWidget::EditSelected);
  connect(m_remove_button, &QPushButton::clicked, this, &ARCodeWidget::RemoveSelected);
  connect(m_toggle_all_button, &QPushButton::clicked, this, &ARCodeWidget::ToggleAll);
}

void ARCodeWidget::ReadCodes()
{
  const bool have_game = !m_game_id.empty();
  if (have_game)
  {
    // Not LoadLocalGameIni(): codes added here are only ever written to the
    // user INI named after the full game ID.
    Common::IniFile user_ini;
    user_ini.Load(UserIniPath(m_game_id));
    const Common::IniFile default_ini = SConfig::LoadDefaultGameIni(m_game_id, m_game_revision);
    m_codes = ActionReplay::LoadCodes(default_ini, user_ini);
  }

  m_list->setEnabled(have_game);
  m_add_button->setEnabled(have_game);
  m_edit_button->setEnabled(false);
  m_remove_button->setEnabled(false);

  RebuildList();
}

void ARCodeWidget::WriteCodes()
{
  if (m_game_id.empty())
    return;

  const std::string path = UserIniPath(m_game_id);
  Common::IniFile user_ini;
  user_ini.Load(path);
  ActionReplay::SaveCodes(&user_ini, m_codes);
  user_ini.Save(path);
}

void ARCodeWidget::RebuildList()
{
  m_list->clear();

  for (size_t index = 0; index < m_codes.size(); ++index)
  {
    const ActionReplay::ARCode& code = m_codes[index];
    auto* const item = new QListWidgetItem(DisplayName(code.name));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
                   Qt::ItemIsDragEnabled);
    item->setCheckState(code.enabled ? Qt::Checked : Qt::Unchecked);
    // Remembers where the code sat in m_codes, for drag reordering.
    item->setData(Qt::UserRole, static_cast<int>(index));

#ifdef USE_RETRO_ACHIEVEMENTS
    const AchievementManager& achievements = AchievementManager::GetInstance();
    if (achievements.IsHardcoreModeActive())
    {
      const QIcon approved = style()->standardIcon(QStyle::SP_DialogYesButton);
      const QIcon unapproved = style()->standardIcon(QStyle::SP_MessageBoxWarning);
      const bool ok = achievements.IsApprovedARCode(code, m_game_id, m_game_revision);
      item->setIcon(ok ? approved : unapproved);
    }
#endif  // USE_RETRO_ACHIEVEMENTS

    m_list->addItem(item);
  }

  m_list->setDragDropMode(QAbstractItemView::InternalMove);
  RefreshToggleAllButton();
}

void ARCodeWidget::ApplyIfRunning()
{
  // With a restart required, the codes are picked up at the next boot.
  if (!m_restart_required)
    ActionReplay::ApplyCodes(m_codes, m_game_id, m_game_revision);
}

bool ARCodeWidget::AllEnabled() const
{
  return std::ranges::all_of(m_codes, &ActionReplay::ARCode::enabled);
}

void ARCodeWidget::RefreshToggleAllButton()
{
  m_toggle_all_button->setText(AllEnabled() ? tr("Disable All") : tr("Enable All"));
  m_toggle_all_button->setDisabled(m_codes.empty());
}

void ARCodeWidget::OnCheckToggled(QListWidgetItem* item)
{
  m_codes[static_cast<size_t>(m_list->row(item))].enabled = item->checkState() == Qt::Checked;
  ApplyIfRunning();
  RefreshToggleAllButton();
  WriteCodes();
}

void ARCodeWidget::OnSelectionChanged()
{
  const QList<QListWidgetItem*> selection = m_list->selectedItems();
  const bool nothing = selection.empty();
  m_edit_button->setDisabled(nothing);
  m_remove_button->setDisabled(nothing);
  if (nothing)
    return;

  // Built-in codes cannot be removed, and editing one makes a user copy.
  const bool own = m_codes[static_cast<size_t>(m_list->row(selection[0]))].user_defined;
  m_remove_button->setEnabled(own);
  m_edit_button->setText(own ? tr("&Edit Code...") : tr("Clone and &Edit Code..."));
}

void ARCodeWidget::OnRowsMoved()
{
  // Put m_codes in the list's new order, using the index stored in each
  // item when the list was built.
  std::vector<ActionReplay::ARCode> reordered;
  reordered.reserve(m_codes.size());
  const int rows = m_list->count();
  for (int row = 0; row < rows; ++row)
  {
    const int from = m_list->item(row)->data(Qt::UserRole).toInt();
    reordered.push_back(std::move(m_codes[static_cast<size_t>(from)]));
  }
  m_codes = std::move(reordered);

  WriteCodes();
}

void ARCodeWidget::ShowSortMenu()
{
  QMenu menu;
  menu.addAction(tr("Sort Alphabetically"), this, &ARCodeWidget::SortByName);
  menu.addAction(tr("Show Enabled Codes First"), this, &ARCodeWidget::PutEnabledFirst);
  menu.addAction(tr("Show Disabled Codes First"), this, &ARCodeWidget::PutDisabledFirst);
  menu.exec(QCursor::pos());
}

void ARCodeWidget::SortByName()
{
  m_list->sortItems();
  OnRowsMoved();
}

void ARCodeWidget::PutEnabledFirst()
{
  std::ranges::stable_partition(m_codes, std::identity{}, &ActionReplay::ARCode::enabled);
  RebuildList();
  WriteCodes();
}

void ARCodeWidget::PutDisabledFirst()
{
  std::ranges::stable_partition(m_codes, std::logical_not{}, &ActionReplay::ARCode::enabled);
  RebuildList();
  WriteCodes();
}

void ARCodeWidget::AddCode(ActionReplay::ARCode code)
{
  m_codes.push_back(std::move(code));
  RebuildList();
  WriteCodes();
}

void ARCodeWidget::NewCode()
{
  ActionReplay::ARCode fresh;
  fresh.enabled = false;

  m_editor->SetARCode(&fresh);
  if (m_editor->exec() == QDialog::Rejected)
    return;

  AddCode(std::move(fresh));
}

void ARCodeWidget::EditSelected()
{
  const QList<QListWidgetItem*> selection = m_list->selectedItems();
  if (selection.empty())
    return;

  const QListWidgetItem* const item = selection[0];
  const bool was_enabled = item->checkState() == Qt::Checked;
  ActionReplay::ARCode& code = m_codes[static_cast<size_t>(m_list->row(item))];

  if (code.user_defined)
  {
    // The editor writes straight into the stored code.
    m_editor->SetARCode(&code);
    if (m_editor->exec() == QDialog::Rejected)
      return;
  }
  else
  {
    // Built-in codes are edited as a copy appended to the list.
    ActionReplay::ARCode copy = code;
    m_editor->SetARCode(&copy);
    if (m_editor->exec() == QDialog::Rejected)
      return;
    m_codes.push_back(std::move(copy));
  }

  WriteCodes();
  RebuildList();

  if (was_enabled)
    ApplyIfRunning();
}

void ARCodeWidget::RemoveSelected()
{
  const QList<QListWidgetItem*> selection = m_list->selectedItems();
  if (selection.empty())
    return;

  m_codes.erase(m_codes.begin() + m_list->row(selection[0]));

  WriteCodes();
  RebuildList();
  m_remove_button->setEnabled(false);
}

void ARCodeWidget::ToggleAll()
{
  const bool enable = !AllEnabled();
  const Qt::CheckState state = enable ? Qt::Checked : Qt::Unchecked;

  {
    // Blocked so that each item's check change doesn't save the INI again.
    const QSignalBlocker quiet(m_list);
    const int count = static_cast<int>(m_codes.size());
    for (int row = 0; row < count; ++row)
    {
      m_codes[static_cast<size_t>(row)].enabled = enable;
      m_list->item(row)->setCheckState(state);
    }
  }

  ApplyIfRunning();
  RebuildList();
  WriteCodes();
}
