// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/GeckoCodeWidget.h"

#include <algorithm>
#include <functional>
#include <utility>

#include <QCursor>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#ifdef USE_RETRO_ACHIEVEMENTS
#include <QIcon>
#include <QStyle>
#endif  // USE_RETRO_ACHIEVEMENTS
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/AchievementManager.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "DolphinQt/Config/CheatCodeEditor.h"
#include "DolphinQt/Config/CheatWarningWidget.h"
#include "DolphinQt/Config/HardcoreWarningWidget.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"
#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Settings.h"
#endif  // USE_RETRO_ACHIEVEMENTS

namespace
{
// Codes added through this tab always go to GameSettings/<id>.ini in the
// user directory.
std::string UserIniPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
}

// Each list item stores the position of its code in the code vector.
size_t CodeIndex(const QListWidgetItem* item)
{
  return static_cast<size_t>(item->data(Qt::UserRole).toInt());
}
}  // namespace

GeckoCodeWidget::GeckoCodeWidget(std::string game_id, std::string gametdb_id, u16 game_revision,
                                 bool restart_required)
    : m_game_id(std::move(game_id)), m_gametdb_id(std::move(gametdb_id)),
      m_game_revision(game_revision), m_restart_required(restart_required)
{
  BuildLayout();
  WireUp();
  ReadCodes();
}

GeckoCodeWidget::~GeckoCodeWidget() = default;

void GeckoCodeWidget::ChangeGame(std::string game_id, std::string gametdb_id,
                                 const u16 game_revision)
{
  m_game_id = std::move(game_id);
  m_gametdb_id = std::move(gametdb_id);
  m_game_revision = game_revision;
  m_restart_required = false;

  m_codes.clear();
  m_list->clear();
  m_name->clear();
  m_creator->clear();
  m_notes->clear();
  m_lines->clear();

  // An open editor would save into the previous game's codes; close it.
  m_editor->reject();

  ReadCodes();
}

void GeckoCodeWidget::BuildLayout()
{
  m_cheat_warning = new CheatWarningWidget(m_game_id, m_restart_required, this);
#ifdef USE_RETRO_ACHIEVEMENTS
  m_hardcore_warning = new HardcoreWarningWidget(this);
#endif  // USE_RETRO_ACHIEVEMENTS
  m_list = new QtUtils::MinimumSizeHintWidget<QListWidget>;
  m_name = new QLabel;
  m_creator = new QLabel;

  m_list->setContextMenuPolicy(Qt::CustomContextMenu);

  // Read-only views of the selected code: 5 lines of notes, 10 of code.
  const QFont fixed(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  const int line = QFontMetrics(font()).lineSpacing();
  m_notes = new QTextEdit;
  m_notes->setFont(fixed);
  m_notes->setReadOnly(true);
  m_notes->setFixedHeight(line * 5);
  m_lines = new QTextEdit;
  m_lines->setFont(fixed);
  m_lines->setReadOnly(true);
  m_lines->setFixedHeight(line * 10);

  m_add_button = new NonDefaultQPushButton(tr("&Add New Code..."));
  m_edit_button = new NonDefaultQPushButton(tr("&Edit Code..."));
  m_remove_button = new NonDefaultQPushButton(tr("&Remove Code"));
  m_download_button = new NonDefaultQPushButton(tr("Download Codes"));
  m_toggle_all_button = new NonDefaultQPushButton(tr("Disable All"));
  m_editor = new CheatCodeEditor(this);

  m_download_button->setToolTip(tr("Download Codes from the WiiRD Database"));

  auto* const column = new QVBoxLayout{this};
  column->addWidget(m_cheat_warning);
#ifdef USE_RETRO_ACHIEVEMENTS
  column->addWidget(m_hardcore_warning);
#endif  // USE_RETRO_ACHIEVEMENTS
  column->addWidget(m_list);

  auto* const details = new QFormLayout;
  details->addRow(tr("Name:"), m_name);
  details->addRow(tr("Creator:"), m_creator);
  // The description row only holds its caption; the notes box sits below.
  details->addRow(tr("Description:"), static_cast<QWidget*>(nullptr));
  details->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  for (QLabel* const value : {m_name, m_creator})
  {
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setCursor(Qt::IBeamCursor);
  }

  column->addLayout(details);
  column->addWidget(m_notes);
  column->addWidget(m_lines);

  auto* const buttons = new QHBoxLayout;
  for (QPushButton* const button : {m_add_button, m_edit_button, m_remove_button,
                                    m_download_button, m_toggle_all_button})
  {
    buttons->addWidget(button);
  }
  column->addLayout(buttons);
}

void GeckoCodeWidget::WireUp()
{
  connect(m_list, &QListWidget::itemSelectionChanged, this,
          &GeckoCodeWidget::ShowSelectedDetails);
  connect(m_list, &QListWidget::itemChanged, this, &GeckoCodeWidget::OnCheckToggled);
  connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, &GeckoCodeWidget::OnRowsMoved);
  connect(m_list, &QListWidget::customContextMenuRequested, this, &GeckoCodeWidget::ShowSortMenu);

  connect(m_add_button, &QPushButton::clicked, this, &GeckoCodeWidget::NewCode);
  connect(m_remove_button, &QPushButton::clicked, this, &GeckoCodeWidget::RemoveCurrent);
  connect(m_edit_button, &QPushButton::clicked, this, &GeckoCodeWidget::EditCurrent);
  connect(m_download_button, &QPushButton::clicked, this, &GeckoCodeWidget::FetchFromServer);
  connect(m_toggle_all_button, &QPushButton::clicked, this, &GeckoCodeWidget::ToggleAll);
  connect(m_cheat_warning, &CheatWarningWidget::OpenCheatEnableSettings, this,
          &GeckoCodeWidget::OpenGeneralSettings);
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_hardcore_warning, &HardcoreWarningWidget::OpenAchievementSettings, this,
          &GeckoCodeWidget::OpenAchievementSettings);
  // Approval icons follow hardcore mode, which can change with the
  // emulation state.
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &GeckoCodeWidget::RebuildList);
#endif  // USE_RETRO_ACHIEVEMENTS
}

void GeckoCodeWidget::ReadCodes()
{
  const bool have_game = !m_game_id.empty();
  if (have_game)
  {
    // Not LoadLocalGameIni(): this tab only ever writes the INI named after
    // the full game ID.
    Common::IniFile user_ini;
    user_ini.Load(UserIniPath(m_game_id));
    const Common::IniFile default_ini = SConfig::LoadDefaultGameIni(m_game_id, m_game_revision);
    m_codes = Gecko::LoadCodes(default_ini, user_ini);
  }

  m_list->setEnabled(have_game);
  m_name->setEnabled(have_game);
  m_creator->setEnabled(have_game);
  m_notes->setEnabled(have_game);
  m_lines->setEnabled(have_game);

  m_add_button->setEnabled(have_game);
  m_edit_button->setEnabled(false);
  m_remove_button->setEnabled(false);
  m_download_button->setEnabled(have_game);

  RebuildList();
}

void GeckoCodeWidget::WriteCodes()
{
  if (m_game_id.empty())
    return;

  const std::string path = UserIniPath(m_game_id);
  Common::IniFile user_ini;
  user_ini.Load(path);
  Gecko::SaveCodes(user_ini, m_codes);
  user_ini.Save(path);
}

void GeckoCodeWidget::RebuildList()
{
  m_list->clear();

  for (size_t index = 0; index < m_codes.size(); ++index)
  {
    const Gecko::GeckoCode& code = m_codes[index];
    // Stored names escape < and >.
    const QString label = QString::fromStdString(code.name)
                              .replace(QStringLiteral("&lt;"), QChar::fromLatin1('<'))
                              .replace(QStringLiteral("&gt;"), QChar::fromLatin1('>'));
    auto* const item = new QListWidgetItem(label);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
                   Qt::ItemIsDragEnabled);
    item->setCheckState(code.enabled ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, static_cast<int>(index));

#ifdef USE_RETRO_ACHIEVEMENTS
    const AchievementManager& achievements = AchievementManager::GetInstance();
    if (achievements.IsHardcoreModeActive())
    {
      const QIcon approved = style()->standardIcon(QStyle::SP_DialogYesButton);
      const QIcon unapproved = style()->standardIcon(QStyle::SP_MessageBoxWarning);
      const bool ok = achievements.IsApprovedGeckoCode(code, m_game_id, m_game_revision);
      item->setIcon(ok ? approved : unapproved);
    }
#endif  // USE_RETRO_ACHIEVEMENTS

    m_list->addItem(item);
  }

  m_list->setDragDropMode(QAbstractItemView::InternalMove);
  RefreshToggleAllButton();
}

void GeckoCodeWidget::ActivateIfRunning()
{
  // With a restart required, the codes are picked up at the next boot.
  if (!m_restart_required)
    Gecko::SetActiveCodes(m_codes, m_game_id, m_game_revision);
}

bool GeckoCodeWidget::AllEnabled() const
{
  return std::ranges::all_of(m_codes, &Gecko::GeckoCode::enabled);
}

void GeckoCodeWidget::RefreshToggleAllButton()
{
  m_toggle_all_button->setText(AllEnabled() ? tr("Disable All") : tr("Enable All"));
  m_toggle_all_button->setDisabled(m_codes.empty());
}

void GeckoCodeWidget::ShowSelectedDetails()
{
  const QList<QListWidgetItem*> selection = m_list->selectedItems();
  const bool nothing = selection.empty();
  m_edit_button->setDisabled(nothing);
  m_remove_button->setDisabled(nothing);
  if (nothing)
    return;

  const Gecko::GeckoCode& code = m_codes[CodeIndex(selection[0])];
  m_name->setText(QString::fromStdString(code.name));
  m_creator->setText(QString::fromStdString(code.creator));

  m_notes->clear();
  for (const std::string& note : code.notes)
    m_notes->append(QString::fromStdString(note));

  m_lines->clear();
  for (const Gecko::GeckoCode::Code& entry : code.codes)
    m_lines->append(QString::fromStdString(entry.original_line));
}

void GeckoCodeWidget::OnCheckToggled(QListWidgetItem* item)
{
  m_codes[CodeIndex(item)].enabled = item->checkState() == Qt::Checked;
  ActivateIfRunning();
  RefreshToggleAllButton();
  WriteCodes();
}

void GeckoCodeWidget::OnRowsMoved()
{
  // Rebuild the vector in the list's new order.
  std::vector<Gecko::GeckoCode> reordered;
  reordered.reserve(m_codes.size());
  const int rows = m_list->count();
  for (int row = 0; row < rows; ++row)
    reordered.push_back(std::move(m_codes[CodeIndex(m_list->item(row))]));
  m_codes = std::move(reordered);

  RebuildList();
  WriteCodes();
}

void GeckoCodeWidget::ShowSortMenu()
{
  QMenu menu;
  menu.addAction(tr("Sort Alphabetically"), this, &GeckoCodeWidget::SortByName);
  menu.addAction(tr("Show Enabled Codes First"), this, &GeckoCodeWidget::PutEnabledFirst);
  menu.addAction(tr("Show Disabled Codes First"), this, &GeckoCodeWidget::PutDisabledFirst);
  menu.exec(QCursor::pos());
}

void GeckoCodeWidget::SortByName()
{
  m_list->sortItems();
  OnRowsMoved();
}

void GeckoCodeWidget::PutEnabledFirst()
{
  std::ranges::stable_partition(m_codes, std::identity{}, &Gecko::GeckoCode::enabled);
  RebuildList();
  WriteCodes();
}

void GeckoCodeWidget::PutDisabledFirst()
{
  std::ranges::stable_partition(m_codes, std::logical_not{}, &Gecko::GeckoCode::enabled);
  RebuildList();
  WriteCodes();
}

void GeckoCodeWidget::NewCode()
{
  Gecko::GeckoCode fresh;
  fresh.enabled = false;

  m_editor->SetGeckoCode(&fresh);
  if (m_editor->exec() == QDialog::Rejected)
    return;

  m_codes.push_back(std::move(fresh));
  WriteCodes();
  RebuildList();
}

void GeckoCodeWidget::EditCurrent()
{
  const QListWidgetItem* const item = m_list->currentItem();
  if (item == nullptr)
    return;

  const size_t index = CodeIndex(item);
  const bool was_enabled = item->checkState() == Qt::Checked;

  // The editor writes into the stored code directly.
  m_editor->SetGeckoCode(&m_codes[index]);
  if (m_editor->exec() == QDialog::Rejected)
    return;

  WriteCodes();
  RebuildList();

  if (was_enabled)
    ActivateIfRunning();
}

void GeckoCodeWidget::RemoveCurrent()
{
  const QListWidgetItem* const item = m_list->currentItem();
  if (item == nullptr)
    return;

  m_codes.erase(m_codes.begin() + static_cast<std::ptrdiff_t>(CodeIndex(item)));
  RebuildList();
  WriteCodes();
}

void GeckoCodeWidget::FetchFromServer()
{
  const auto downloaded = Gecko::DownloadCodes(m_gametdb_id);
  if (!downloaded)
  {
    QString failure = tr("Failed to download Gecko codes. The code server may be "
                         "temporarily unavailable. Please try again later.");
    // A positive error is the server's HTTP status.
    const int status = downloaded.error();
    if (status > 0)
      failure += tr("\n\nServer response: HTTP %1.").arg(status);
    ModalMessageBox::critical(this, tr("Download Failed"), failure);
    return;
  }

  const std::vector<Gecko::GeckoCode>& fetched = *downloaded;
  if (fetched.empty())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("File contained no codes."));
    return;
  }

  // Only codes not already in the list are added.
  size_t added = 0;
  for (const Gecko::GeckoCode& code : fetched)
  {
    if (std::ranges::find(m_codes, code) != m_codes.end())
      continue;
    m_codes.push_back(code);
    ++added;
  }

  RebuildList();
  WriteCodes();

  ModalMessageBox::information(this, tr("Download complete"),
                               tr("Downloaded %1 codes. (added %2)")
                                   .arg(QString::number(fetched.size()), QString::number(added)));
}

void GeckoCodeWidget::ToggleAll()
{
  const bool enable = !AllEnabled();
  const Qt::CheckState state = enable ? Qt::Checked : Qt::Unchecked;

  {
    // Blocked so each item's check change doesn't save the INI again.
    const QSignalBlocker quiet(m_list);
    const int count = static_cast<int>(m_codes.size());
    for (int row = 0; row < count; ++row)
    {
      m_codes[static_cast<size_t>(row)].enabled = enable;
      m_list->item(row)->setCheckState(state);
    }
  }

  ActivateIfRunning();
  RebuildList();
  WriteCodes();
}
