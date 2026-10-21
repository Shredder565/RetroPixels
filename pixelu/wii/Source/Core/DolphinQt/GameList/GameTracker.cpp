// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/GameList/GameTracker.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>

#include "Common/Config/Config.h"

#include "Core/Config/UISettings.h"

#include "DiscIO/DirectoryBlob.h"

#include "DolphinQt/QtUtils/QueueOnObject.h"

#include "DolphinQt/Settings.h"

#include "UICommon/GameFile.h"

namespace
{
// QDirIterator matches name filters case-sensitively on some platforms, so each extension is
// spelled as a bracket pattern that accepts both cases of every letter ("gcm" -> "*.[gG][cC][mM]").
QString AnyCaseExtension(const char* extension)
{
  QString pattern = QStringLiteral("*.");
  for (const char* c = extension; *c != '\0'; ++c)
  {
    const QChar letter = QLatin1Char(*c);
    pattern += QLatin1Char('[');
    pattern += letter.toLower();
    pattern += letter.toUpper();
    pattern += QLatin1Char(']');
  }
  return pattern;
}

const QStringList& GameFileNameFilters()
{
  static const QStringList filters = [] {
    QStringList list;
    for (const char* extension : {"gcm", "bin", "iso", "tgc", "ciso", "gcz", "wbfs", "wia", "rvz"})
      list.push_back(AnyCaseExtension(extension));
    // The main file of an extracted Wii U vWii title has a fixed name.
    list.push_back(QStringLiteral("hif_000000.nfs"));
    for (const char* extension : {"wad", "elf", "dol", "json"})
      list.push_back(AnyCaseExtension(extension));
    return list;
  }();
  return filters;
}

// Walks the game files in a folder, including its subfolders if the user asked for that.
std::unique_ptr<QDirIterator> ScanFolder(const QString& dir)
{
  const bool recursive = Config::Get(Config::MAIN_RECURSIVE_ISO_PATHS);
  return std::make_unique<QDirIterator>(
      dir, GameFileNameFilters(), QDir::NoFilter,
      recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
}

QString NextCanonicalPath(QDirIterator& it)
{
  return QFileInfo(it.next()).canonicalFilePath();
}
}  // namespace

GameTracker::GameTracker(QObject* parent) : QFileSystemWatcher(parent)
{
  qRegisterMetaType<std::shared_ptr<const UICommon::GameFile>>();
  qRegisterMetaType<std::string>();

  auto& settings = Settings::Instance();

  connect(qApp, &QApplication::aboutToQuit, this, [this] {
    m_halted = true;
    m_worker.StopAndCancel();
  });
  connect(this, &QFileSystemWatcher::directoryChanged, this,
          [this](const QString& dir) { Post(Job::RescanFolder, dir); });
  connect(this, &QFileSystemWatcher::fileChanged, this,
          [this](const QString& path) { Post(Job::RescanFile, path); });

  // Turning auto refresh on or off re-registers every folder, which (un)watches them as needed.
  connect(&settings, &Settings::AutoRefreshToggled, this, [] {
    auto& s = Settings::Instance();
    for (const QString& dir : s.GetPaths())
    {
      s.RemovePath(dir);
      s.AddPath(dir);
    }
  });
  connect(&settings, &Settings::MetadataRefreshRequested, this,
          [this] { Post(Job::RefreshMetadata); });

  m_worker.Reset("GameList Tracker", [this](const Task& task) { RunTask(task); });
  Post(Job::ReadCache);
}

void GameTracker::Post(Job job, const QString& path)
{
  m_worker.EmplaceItem(Task{job, path});
}

void GameTracker::RunTask(const Task& task)
{
  const auto notify = [this](void (Settings::*method)()) {
    QueueOnObject(this, [method] { (Settings::Instance().*method)(); });
  };

  switch (task.job)
  {
  case Job::ReadCache:
    ReadCache();
    return;
  case Job::FirstScan:
    FirstScan();
    return;
  case Job::AddFolder:
    AddFolder(task.path);
    return;
  case Job::DropFolder:
    DropFolder(task.path);
    return;
  case Job::RescanFolder:
    RescanFolder(task.path);
    return;
  case Job::RescanFile:
    RescanFile(task.path);
    return;
  case Job::RefreshMetadata:
    m_cache.UpdateAdditionalMetadata(
        [this](const std::shared_ptr<const UICommon::GameFile>& game) { emit GameUpdated(game); },
        m_halted);
    notify(&Settings::NotifyMetadataRefreshComplete);
    return;
  case Job::Resume:
    m_halted = false;
    return;
  case Job::ClearCache:
    m_cache.Clear(UICommon::GameFileCache::DeleteOnDisk::Yes);
    return;
  case Job::RefreshBegin:
  {
    m_refreshing = true;
    notify(&Settings::NotifyRefreshGameListStarted);
    // Everything is dropped here and found again by the folder jobs queued after this one.
    const QList<QString> known = m_owners.keys();
    for (const QString& path : known)
      emit GameRemoved(path.toStdString());
    m_owners.clear();
    return;
  }
  case Job::RefreshEnd:
    m_refreshing = false;
    m_cache.Save();
    notify(&Settings::NotifyRefreshGameListComplete);
    return;
  }
}

void GameTracker::ReadCache()
{
  m_cache.Load();
  m_cache_ready.Set();
}

void GameTracker::Start()
{
  if (m_cached_games_queued)
    return;
  m_cached_games_queued = true;

  Post(Job::FirstScan);

  // Hand out the cached games right away, from this thread; the worker holds off its first scan
  // until they are all out (see FirstScan).
  m_cache_ready.Wait();
  m_cache.ForEach(
      [this](const std::shared_ptr<const UICommon::GameFile>& game) { emit GameLoaded(game); });
  m_cached_games_sent.Set();
}

void GameTracker::FirstScan()
{
  if (m_scanning_enabled)
    return;
  m_scanning_enabled = true;

  QueueOnObject(this, [] { Settings::Instance().NotifyRefreshGameListStarted(); });

  std::vector<std::string> known_paths;
  known_paths.reserve(m_owners.size());
  for (auto it = m_owners.keyBegin(); it != m_owners.keyEnd(); ++it)
    known_paths.push_back(it->toStdString());

  m_cached_games_sent.Wait();

  const auto on_added = [this](const std::shared_ptr<const UICommon::GameFile>& game) {
    emit GameLoaded(game);
  };
  const auto on_removed = [this](const std::string& path) { emit GameRemoved(path); };
  const auto on_updated = [this](const std::shared_ptr<const UICommon::GameFile>& game) {
    emit GameUpdated(game);
  };

  const bool list_changed = m_cache.Update(known_paths, on_added, on_removed, m_halted);
  const bool metadata_changed = m_cache.UpdateAdditionalMetadata(on_updated, m_halted);
  if (list_changed || metadata_changed)
    m_cache.Save();

  QueueOnObject(this, [] { Settings::Instance().NotifyMetadataRefreshComplete(); });
  QueueOnObject(this, [] { Settings::Instance().NotifyRefreshGameListComplete(); });
}

bool GameTracker::Watch(const QString& path)
{
  // QFileSystemWatcher belongs to the GUI thread, so the actual (un)watching is queued there.
  if (Settings::Instance().IsAutoRefreshEnabled())
    QueueOnObject(this, [this, path] { return addPath(path); });

  m_watched.push_back(path);
  return true;
}

bool GameTracker::Unwatch(const QString& path)
{
  if (Settings::Instance().IsAutoRefreshEnabled())
    QueueOnObject(this, [this, path] { return removePath(path); });

  const qsizetype position = m_watched.indexOf(path);
  if (position < 0)
    return false;

  m_watched.removeAt(position);
  return true;
}

void GameTracker::AddDirectory(const QString& dir)
{
  Post(Job::AddFolder, dir);
}

void GameTracker::RemoveDirectory(const QString& dir)
{
  Post(Job::DropFolder, dir);
}

void GameTracker::RefreshAll()
{
  // Abandon whatever is in progress or queued, then rebuild the list from scratch.
  m_halted = true;
  m_worker.Cancel();
  Post(Job::Resume);

  if (m_purge_on_refresh)
  {
    Post(Job::ClearCache);
    m_purge_on_refresh = false;
  }

  Post(Job::RefreshBegin);
  for (const QString& dir : Settings::Instance().GetPaths())
  {
    Post(Job::DropFolder, dir);
    Post(Job::AddFolder, dir);
  }
  Post(Job::RefreshEnd);
}

void GameTracker::AddFolder(const QString& dir)
{
  if (!QFileInfo(dir).exists())
    return;

  Watch(dir);
  RescanFolder(dir);
}

void GameTracker::DropFolder(const QString& dir)
{
  Unwatch(dir);

  const auto files = ScanFolder(dir);
  while (files->hasNext())
  {
    const QString path = NextCanonicalPath(*files);
    const auto owner = m_owners.find(path);
    if (owner == m_owners.end())
      continue;

    owner->remove(dir);
    if (!owner->isEmpty())
      continue;

    // No other folder still provides this game.
    Unwatch(path);
    m_owners.erase(owner);
    if (m_scanning_enabled)
      emit GameRemoved(path.toStdString());
  }
}

void GameTracker::RescanFolder(const QString& dir)
{
  const auto files = ScanFolder(dir);
  while (files->hasNext() && !m_halted)
  {
    const QString path = NextCanonicalPath(*files);

    const auto owner = m_owners.find(path);
    if (owner != m_owners.end())
    {
      owner->insert(dir);
      continue;
    }

    Watch(path);
    m_owners[path] = QSet<QString>{dir};
    LoadGame(path);
  }

  const QSet<QString> gone = GamesGoneFrom(dir);
  for (const QString& path : gone)
  {
    if (m_halted)
      break;

    QSet<QString>& folders = m_owners[path];
    folders.remove(dir);
    if (!folders.empty())
      continue;

    m_owners.remove(path);
    if (m_scanning_enabled)
      emit GameRemoved(path.toStdString());
  }
}

void GameTracker::RescanFile(const QString& path)
{
  if (QFileInfo(path).exists())
  {
    // Changed on disk: drop the old entry and load the file again.
    if (m_scanning_enabled)
      emit GameRemoved(path.toStdString());
    Watch(path);
    LoadGame(path);
    return;
  }

  if (!Unwatch(path))
    return;

  m_owners.remove(path);
  if (m_scanning_enabled)
    emit GameRemoved(path.toStdString());
}

QSet<QString> GameTracker::GamesGoneFrom(const QString& dir)
{
  // Start from every game this folder provided, then strike off the ones still in it.
  QSet<QString> gone;
  for (auto it = m_owners.cbegin(); it != m_owners.cend(); ++it)
  {
    if (it.value().contains(dir))
      gone.insert(it.key());
  }

  const auto files = ScanFolder(dir);
  while (files->hasNext())
  {
    const QString path = NextCanonicalPath(*files);
    if (m_owners.contains(path))
      gone.remove(path);
  }

  return gone;
}

void GameTracker::LoadGame(const QString& path)
{
  if (!m_scanning_enabled)
    return;

  const std::string file_path = path.toStdString();
  if (DiscIO::ShouldHideFromGameList(file_path))
    return;

  bool cache_changed = false;
  const std::shared_ptr<const UICommon::GameFile> game = m_cache.AddOrGet(file_path, &cache_changed);
  if (game)
    emit GameLoaded(game);

  // During a refresh the cache is written once, at the end.
  if (cache_changed && !m_refreshing)
    m_cache.Save();
}

void GameTracker::PurgeCache()
{
  m_purge_on_refresh = true;
  Settings::Instance().RefreshGameList();
}
