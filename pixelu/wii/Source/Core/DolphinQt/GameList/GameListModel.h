// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>

#include <QAbstractTableModel>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "Core/Core.h"
#include "Core/TimePlayed.h"
#include "Core/TitleDatabase.h"

#include "DolphinQt/GameList/GameTracker.h"

namespace UICommon
{
class GameFile;
}

// The game list's data: one row per game file, one column per property. The list and grid
// views look at it through their own proxy models.
class GameListModel final : public QAbstractTableModel
{
  Q_OBJECT

public:
  explicit GameListModel(QObject* parent = nullptr);

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;
  int rowCount(const QModelIndex& parent) const override;
  int columnCount(const QModelIndex& parent) const override;

  std::shared_ptr<const UICommon::GameFile> GetGameFile(int index) const;
  std::string GetNetPlayName(const UICommon::GameFile& game) const;
  bool ShouldDisplayGameListItem(int index) const;
  void SetSearchTerm(const QString& term);

  // The proxies sort by this role. It mostly matches Qt::DisplayRole, but uses raw numbers for
  // sizes and times and pads the numbers inside titles.
  static constexpr int SORT_ROLE = Qt::UserRole;

  enum class Column
  {
    Platform = 0,
    Banner,
    Title,
    Description,
    Maker,
    ID,
    Country,
    Size,
    FileName,
    FilePath,
    FileFormat,
    BlockSize,
    Compression,
    TimePlayed,
    Tags,
    Count,
  };

  void AddGame(const std::shared_ptr<const UICommon::GameFile>& game);
  void UpdateGame(const std::shared_ptr<const UICommon::GameFile>& game);
  void RemoveGame(const std::string& path);

  std::shared_ptr<const UICommon::GameFile> FindGame(const std::string& path) const;
  std::shared_ptr<const UICommon::GameFile> FindSecondDisc(const UICommon::GameFile& game) const;

  void SetScale(float scale);
  float GetScale() const;

  const QStringList& GetAllTags() const;
  const QStringList GetGameTags(const std::string& path) const;

  void AddGameTag(const std::string& path, const QString& name);
  void RemoveGameTag(const std::string& path, const QString& name);

  void NewTag(const QString& name);
  void DeleteTag(const QString& name);

  void PurgeCache();

private:
  QVariant TitleData(const UICommon::GameFile& game, int role) const;
  QVariant TimePlayedData(const UICommon::GameFile& game, int role) const;
  int RowOf(const std::string& path) const;
  void SetTagsOf(const std::string& path, const QStringList& tags);
  void StoreTagNames() const;

  QStringList m_tags;
  QMap<QString, QVariant> m_tags_by_path;

  GameTracker m_tracker;
  QList<std::shared_ptr<const UICommon::GameFile>> m_games;
  Core::TitleDatabase m_titles;
  TimePlayed m_time_played;
  QString m_search;
  float m_zoom = 1.0;
};
