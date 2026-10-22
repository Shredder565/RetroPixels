// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <QFileSystemWatcher>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

#include "Common/Event.h"
#include "Common/WorkQueueThread.h"
#include "UICommon/GameFileCache.h"

namespace UICommon
{
class GameFile;
}

// Keeps the game list in step with the game folders. All scanning and game file loading happens
// on a worker thread, which reports through the three signals below.
//
// Nothing is emitted before Start(). Register every folder with AddDirectory() first, so that
// the games from the cache do not vanish for a moment while the first scan catches up.
class GameTracker final : public QFileSystemWatcher
{
  Q_OBJECT

public:
  explicit GameTracker(QObject* parent = nullptr);

  void Start();

  void AddDirectory(const QString& dir);
  void RemoveDirectory(const QString& dir);
  void RefreshAll();
  void PurgeCache();

signals:
  void GameLoaded(const std::shared_ptr<const UICommon::GameFile>& game);
  void GameUpdated(const std::shared_ptr<const UICommon::GameFile>& game);
  void GameRemoved(const std::string& path);

private:
  // What the worker thread is asked to do.
  enum class Job
  {
    ReadCache,
    FirstScan,
    AddFolder,
    DropFolder,
    RescanFolder,
    RescanFile,
    RefreshMetadata,
    Resume,
    ClearCache,
    RefreshBegin,
    RefreshEnd,
  };

  struct Task
  {
    Job job;
    QString path;
  };

  void Post(Job job, const QString& path = {});
  void RunTask(const Task& task);

  // Worker thread side.
  void ReadCache();
  void FirstScan();
  void AddFolder(const QString& dir);
  void DropFolder(const QString& dir);
  void RescanFolder(const QString& dir);
  void RescanFile(const QString& path);
  QSet<QString> GamesGoneFrom(const QString& dir);
  void LoadGame(const QString& path);

  // Bookkeeping of what the file system watcher looks at.
  bool Watch(const QString& path);
  bool Unwatch(const QString& path);

  // For each game file: the folders it was found in.
  QMap<QString, QSet<QString>> m_owners;
  QVector<QString> m_watched;
  UICommon::GameFileCache m_cache;
  Common::Event m_cache_ready;
  Common::Event m_cached_games_sent;
  bool m_cached_games_queued = false;
  bool m_scanning_enabled = false;
  bool m_purge_on_refresh = false;
  bool m_refreshing = false;
  std::atomic_bool m_halted = false;
  // Declared last so that the worker is gone before the state it works on.
  Common::WorkQueueThread<Task> m_worker;
};

Q_DECLARE_METATYPE(std::shared_ptr<const UICommon::GameFile>)
Q_DECLARE_METATYPE(std::string)
