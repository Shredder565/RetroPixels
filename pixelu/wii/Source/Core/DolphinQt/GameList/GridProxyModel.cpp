// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/GameList/GridProxyModel.h"

#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSize>

#include "DolphinQt/GameList/GameListModel.h"

#include "Core/Config/UISettings.h"

#include "UICommon/GameFile.h"

namespace
{
const QSize COVER_SIZE(160, 224);
const QSize BIG_BANNER_SIZE(144, 48);

int TitleColumn()
{
  return static_cast<int>(GameListModel::Column::Title);
}
}  // namespace

GridProxyModel::GridProxyModel(QObject* parent) : QSortFilterProxyModel(parent)
{
  setDynamicSortFilter(true);
}

QVariant GridProxyModel::data(const QModelIndex& index, int role) const
{
  const int row = mapToSource(index).row();

  if (role == Qt::DisplayRole)
    return sourceModel()->data(sourceModel()->index(row, TitleColumn()), Qt::DisplayRole);

  if (role != Qt::DecorationRole)
    return {};

  auto* const games = static_cast<GameListModel*>(sourceModel());
  const float zoom = games->GetScale();
  const auto& cover = games->GetGameFile(row)->GetCoverImage().buffer;
  const bool use_covers = Config::Get(Config::MAIN_USE_GAME_COVERS);

  if (use_covers && !cover.empty())
  {
    const QImage image =
        QImage::fromData(reinterpret_cast<const unsigned char*>(cover.data()),
                         static_cast<int>(cover.size()));
    const QPixmap picture = QPixmap::fromImage(image);
    return picture.scaled(COVER_SIZE * zoom * picture.devicePixelRatio(), Qt::KeepAspectRatio,
                          Qt::SmoothTransformation);
  }

  // No cover to show: center the banner vertically on a white tile the size a cover (or a large
  // banner, when covers are off) would take.
  const QSize tile_size = (use_covers ? COVER_SIZE : BIG_BANNER_SIZE) * zoom;
  QPixmap tile(tile_size * QPixmap().devicePixelRatio());

  const QModelIndex banner_cell = games->index(row, static_cast<int>(GameListModel::Column::Banner));
  const QPixmap banner = games->data(banner_cell, Qt::DecorationRole)
                             .value<QPixmap>()
                             .scaled(tile.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

  tile.fill();
  {
    QPainter painter(&tile);
    const int top = tile.height() / 2 - banner.height() / 2;
    painter.drawPixmap(0, top, banner.width(), banner.height(), banner);
  }
  return tile;
}

bool GridProxyModel::filterAcceptsRow(int source_row, const QModelIndex&) const
{
  const auto* const games = qobject_cast<GameListModel*>(sourceModel());
  return games->ShouldDisplayGameListItem(source_row);
}

bool GridProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
  const QVariant left_key = left.data(GameListModel::SORT_ROLE);
  const QVariant right_key = right.data(GameListModel::SORT_ROLE);
  if (left_key != right_key)
    return QSortFilterProxyModel::lessThan(left, right);

  // Equal keys: order by title instead, in the current sort direction.
  const QString a = sourceModel()->index(left.row(), TitleColumn()).data().toString();
  const QString b = sourceModel()->index(right.row(), TitleColumn()).data().toString();
  if (sortOrder() == Qt::AscendingOrder)
    return a < b;
  return b < a;
}
