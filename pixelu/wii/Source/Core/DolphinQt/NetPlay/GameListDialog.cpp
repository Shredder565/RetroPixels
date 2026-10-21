// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NetPlay/GameListDialog.h"

#include <memory>

#include <QDialogButtonBox>
#include <QListWidget>
#include <QVBoxLayout>

#include "UICommon/GameFile.h"

namespace
{
using GamePtr = std::shared_ptr<const UICommon::GameFile>;

// Each list entry carries its game file so the selection can be handed back directly.
constexpr int GAME_ROLE = Qt::UserRole;
}  // namespace

GameListDialog::GameListDialog(const GameListModel& games, QWidget* owner)
    : QDialog(owner), m_game_list_model(games)
{
  setWindowTitle(tr("Select a game"));

  CreateWidgets();
  ConnectWidgets();
}

void GameListDialog::CreateWidgets()
{
  m_game_list = new QListWidget;

  // Nothing is chosen yet, so there is nothing to confirm.
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Ok);
  m_button_box->setEnabled(false);

  m_main_layout = new QVBoxLayout;
  m_main_layout->addWidget(m_game_list);
  m_main_layout->addWidget(m_button_box);
  setLayout(m_main_layout);
}

void GameListDialog::ConnectWidgets()
{
  connect(m_game_list, &QListWidget::itemSelectionChanged, [this] {
    const bool has_choice = m_game_list->currentRow() != -1;
    m_button_box->setEnabled(has_choice);
  });

  // Double-clicking an entry is a shortcut for selecting it and pressing OK.
  connect(m_game_list, &QListWidget::itemDoubleClicked, this, &GameListDialog::accept);
  connect(m_button_box, &QDialogButtonBox::accepted, this, &GameListDialog::accept);
}

void GameListDialog::PopulateGameList()
{
  m_game_list->clear();

  const int game_count = m_game_list_model.rowCount(QModelIndex());
  for (int row = 0; row < game_count; ++row)
  {
    GamePtr game = m_game_list_model.GetGameFile(row);
    const QString title = QString::fromStdString(m_game_list_model.GetNetPlayName(*game));

    auto* const entry = new QListWidgetItem(title);
    entry->setData(GAME_ROLE, QVariant::fromValue(std::move(game)));
    m_game_list->addItem(entry);
  }

  m_game_list->sortItems();
}

const UICommon::GameFile& GameListDialog::GetSelectedGame() const
{
  const QListWidgetItem* const chosen = m_game_list->selectedItems()[0];
  return *chosen->data(GAME_ROLE).value<GamePtr>();
}

int GameListDialog::exec()
{
  // Rebuilt on every opening so games added since the last time show up.
  PopulateGameList();
  return QDialog::exec();
}
