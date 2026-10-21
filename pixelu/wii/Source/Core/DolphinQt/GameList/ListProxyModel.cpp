// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/GameList/ListProxyModel.h"

#include "DolphinQt/GameList/GameListModel.h"

ListProxyModel::ListProxyModel(QObject* parent) : QSortFilterProxyModel(parent)
{
  setDynamicSortFilter(true);
}

bool ListProxyModel::filterAcceptsRow(int source_row, const QModelIndex&) const
{
  return qobject_cast<GameListModel*>(sourceModel())->ShouldDisplayGameListItem(source_row);
}

bool ListProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
  if (left.data(GameListModel::SORT_ROLE) != right.data(GameListModel::SORT_ROLE))
    return QSortFilterProxyModel::lessThan(left, right);

  // Ties are broken by title, following the direction of the current sort.
  const int title_column = static_cast<int>(GameListModel::Column::Title);
  const QAbstractItemModel* const games = sourceModel();
  const QString left_title = games->index(left.row(), title_column).data().toString();
  const QString right_title = games->index(right.row(), title_column).data().toString();
  return sortOrder() == Qt::AscendingOrder ? left_title < right_title : right_title < left_title;
}
