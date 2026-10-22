// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QSortFilterProxyModel>

// The list view's proxy: every column of GameListModel, with the game list filters applied.
class ListProxyModel final : public QSortFilterProxyModel
{
  Q_OBJECT

public:
  explicit ListProxyModel(QObject* parent = nullptr);
  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

protected:
  bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
};
