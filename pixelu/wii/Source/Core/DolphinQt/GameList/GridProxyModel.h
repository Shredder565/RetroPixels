// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QSortFilterProxyModel>

// The grid view's proxy. Each game shows as one item: its cover or a large banner as the
// decoration and its title as the text.
class GridProxyModel final : public QSortFilterProxyModel
{
  Q_OBJECT

public:
  explicit GridProxyModel(QObject* parent = nullptr);
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

protected:
  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;
  bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
};
