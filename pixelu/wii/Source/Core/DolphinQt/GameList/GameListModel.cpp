// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/GameList/GameListModel.h"

#include <algorithm>
#include <chrono>

#include <QDir>
#include <QFileInfo>
#include <QPixmap>
#include <QRegularExpression>

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/TimePlayed.h"

#include "DiscIO/Enums.h"

#include "DolphinQt/QtUtils/ImageConverter.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"

namespace
{
// Banners are drawn at GameCube banner size; the larger Wii banners get a higher pixel ratio.
const QSize BANNER_SIZE(96, 32);

// Width every run of digits in a title is padded to for sorting, so that "Game 9" < "Game 10".
constexpr int SORT_DIGITS = 10;

const QString TAGS_KEY = QStringLiteral("gamelist/tags");
const QString GAME_TAGS_KEY = QStringLiteral("gamelist/game_tags");

bool IsTextRole(int role)
{
  return role == Qt::DisplayRole || role == GameListModel::SORT_ROLE;
}

const Config::Info<bool>& PlatformFilter(DiscIO::Platform platform, bool* known)
{
  *known = true;
  switch (platform)
  {
  case DiscIO::Platform::GameCubeDisc:
    return Config::MAIN_GAMELIST_LIST_GC;
  case DiscIO::Platform::Triforce:
    return Config::MAIN_GAMELIST_LIST_TRI;
  case DiscIO::Platform::WiiDisc:
    return Config::MAIN_GAMELIST_LIST_WII;
  case DiscIO::Platform::WiiWAD:
    return Config::MAIN_GAMELIST_LIST_WAD;
  case DiscIO::Platform::ELFOrDOL:
    return Config::MAIN_GAMELIST_LIST_ELF_DOL;
  default:
    *known = false;
    return Config::MAIN_GAMELIST_LIST_GC;
  }
}

const Config::Info<bool>& RegionFilter(DiscIO::Country country)
{
  switch (country)
  {
  case DiscIO::Country::Australia:
    return Config::MAIN_GAMELIST_LIST_AUSTRALIA;
  case DiscIO::Country::Europe:
    return Config::MAIN_GAMELIST_LIST_PAL;
  case DiscIO::Country::France:
    return Config::MAIN_GAMELIST_LIST_FRANCE;
  case DiscIO::Country::Germany:
    return Config::MAIN_GAMELIST_LIST_GERMANY;
  case DiscIO::Country::Italy:
    return Config::MAIN_GAMELIST_LIST_ITALY;
  case DiscIO::Country::Japan:
    return Config::MAIN_GAMELIST_LIST_JPN;
  case DiscIO::Country::Korea:
    return Config::MAIN_GAMELIST_LIST_KOREA;
  case DiscIO::Country::Netherlands:
    return Config::MAIN_GAMELIST_LIST_NETHERLANDS;
  case DiscIO::Country::Russia:
    return Config::MAIN_GAMELIST_LIST_RUSSIA;
  case DiscIO::Country::Spain:
    return Config::MAIN_GAMELIST_LIST_SPAIN;
  case DiscIO::Country::Taiwan:
    return Config::MAIN_GAMELIST_LIST_TAIWAN;
  case DiscIO::Country::USA:
    return Config::MAIN_GAMELIST_LIST_USA;
  case DiscIO::Country::World:
    return Config::MAIN_GAMELIST_LIST_WORLD;
  default:
    return Config::MAIN_GAMELIST_LIST_UNKNOWN;
  }
}

// Left-pads every run of digits to SORT_DIGITS characters. A run longer than that is kept whole,
// and scanning resumes SORT_DIGITS characters after the start of the run.
void PadNumbersForSorting(QString* text)
{
  static const QRegularExpression digits(QStringLiteral("\\d+"));
  qsizetype from = 0;
  for (;;)
  {
    const QRegularExpressionMatch run = digits.match(*text, from);
    if (!run.hasMatch())
      return;
    from = run.capturedStart();
    text->replace(from, run.capturedLength(), run.captured().rightJustified(SORT_DIGITS));
    from += SORT_DIGITS;
  }
}

QPixmap BannerPixmap(const UICommon::GameFile& game)
{
  QPixmap banner = ToQPixmap(game.GetBannerImage());
  if (banner.isNull())
    banner = Resources::GetMisc(Resources::MiscID::BannerMissing).pixmap(BANNER_SIZE);

  const qreal x_ratio = static_cast<qreal>(banner.width()) / BANNER_SIZE.width();
  const qreal y_ratio = static_cast<qreal>(banner.height()) / BANNER_SIZE.height();
  banner.setDevicePixelRatio(std::max(x_ratio, y_ratio));
  return banner;
}
}  // namespace

GameListModel::GameListModel(QObject* parent) : QAbstractTableModel(parent)
{
  auto& settings = Settings::Instance();

  connect(&m_tracker, &GameTracker::GameLoaded, this, &GameListModel::AddGame);
  connect(&m_tracker, &GameTracker::GameUpdated, this, &GameListModel::UpdateGame);
  connect(&m_tracker, &GameTracker::GameRemoved, this, &GameListModel::RemoveGame);
  connect(&settings, &Settings::PathAdded, &m_tracker, &GameTracker::AddDirectory);
  connect(&settings, &Settings::PathRemoved, &m_tracker, &GameTracker::RemoveDirectory);
  connect(&settings, &Settings::GameListRefreshRequested, &m_tracker, &GameTracker::RefreshAll);
  connect(&settings, &Settings::TitleDBReloadRequested,
          [this] { m_titles = Core::TitleDatabase(); });
  connect(&settings, &Settings::EmulationStateChanged, this, [this](Core::State state) {
    // A game just ended, so its play time on disk has changed.
    if (state == Core::State::Uninitialized)
      m_time_played.Reload();
  });

  for (const QString& dir : settings.GetPaths())
    m_tracker.AddDirectory(dir);
  m_tracker.Start();

  // A theme change swaps the icons. Announcing a layout change makes the views repaint at once;
  // dataChanged would only take effect once a view gets the focus.
  connect(&settings, &Settings::ThemeChanged, [this] {
    emit layoutAboutToBeChanged();
    emit layoutChanged();
  });

  const QSettings& stored = Settings::GetQSettings();
  m_tags = stored.value(TAGS_KEY).toStringList();
  m_tags_by_path = stored.value(GAME_TAGS_KEY).toMap();
}

QVariant GameListModel::TitleData(const UICommon::GameFile& game, int role) const
{
  QString title = QString::fromStdString(game.GetName(m_titles));

  // Name the disc of multi-disc games, unless the title already does.
  const int disc = game.GetDiscNumber() + 1;
  if (disc > 1 || game.IsTwoDiscGame())
  {
    const QRegularExpression disc_in_title(QStringLiteral("disc ?%1").arg(disc),
                                           QRegularExpression::CaseInsensitiveOption);
    if (!title.contains(disc_in_title))
      title.append(tr(" (Disc %1)").arg(disc));
  }

  if (role == SORT_ROLE)
    PadNumbersForSorting(&title);

  return title;
}

QVariant GameListModel::TimePlayedData(const UICommon::GameFile& game, int role) const
{
  if (!IsTextRole(role))
    return {};

  const std::chrono::milliseconds played = m_time_played.GetTimePlayed(game.GetGameID());
  if (role == SORT_ROLE)
    return static_cast<qlonglong>(played.count());

  const auto hours = std::chrono::duration_cast<std::chrono::hours>(played).count();
  const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(played).count();
  // i18n: A time displayed as hours and minutes
  return tr("%1h %2m").arg(hours).arg(minutes % 60);
}

QVariant GameListModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return {};

  const UICommon::GameFile& game = *m_games[index.row()];
  using Variant = UICommon::GameFile::Variant;

  switch (static_cast<Column>(index.column()))
  {
  case Column::Platform:
    if (role == Qt::DecorationRole)
      return Resources::GetPlatform(game.GetPlatform()).pixmap(32, 32);
    if (role == SORT_ROLE)
      return static_cast<int>(game.GetPlatform());
    return {};

  case Column::Country:
    if (role == Qt::DecorationRole)
      return Resources::GetCountry(game.GetCountry()).pixmap(32, 22);
    if (role == SORT_ROLE)
      return static_cast<int>(game.GetCountry());
    return {};

  case Column::Banner:
    if (role == Qt::DecorationRole)
      return BannerPixmap(game);
    return {};

  case Column::Title:
    return IsTextRole(role) ? TitleData(game, role) : QVariant();

  case Column::ID:
    if (IsTextRole(role))
      return QString::fromStdString(game.GetGameID());
    return {};

  case Column::Description:
    if (IsTextRole(role))
    {
      QString text = QString::fromStdString(game.GetDescription(Variant::LongAndPossiblyCustom));
      return text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    }
    return {};

  case Column::Maker:
    if (IsTextRole(role))
      return QString::fromStdString(game.GetMaker(Variant::LongAndPossiblyCustom));
    return {};

  case Column::FileName:
    if (IsTextRole(role))
      return QString::fromStdString(game.GetFileName());
    return {};

  case Column::FilePath:
    if (IsTextRole(role))
    {
      const QFileInfo file(QString::fromStdString(game.GetFilePath()));
      QString folder = QDir::toNativeSeparators(file.absolutePath());
      if (!folder.endsWith(QDir::separator()))
        folder += QDir::separator();
      return folder;
    }
    return {};

  case Column::Size:
    if (role == SORT_ROLE)
      return static_cast<quint64>(game.GetFileSize());
    if (role == Qt::DisplayRole)
    {
      // An asterisk marks files that are smaller than the disc they hold (compressed files).
      QString size = QString::fromStdString(UICommon::FormatSize(game.GetFileSize()));
      if (game.GetFileSize() != game.GetVolumeSize())
        size += QLatin1Char('*');
      return size;
    }
    return {};

  case Column::FileFormat:
    if (IsTextRole(role))
      return QString::fromStdString(game.GetFileFormatName());
    return {};

  case Column::BlockSize:
    if (role == SORT_ROLE)
      return static_cast<quint64>(game.GetBlockSize());
    if (role == Qt::DisplayRole)
      return QString::fromStdString(UICommon::FormatSize(game.GetBlockSize()));
    return {};

  case Column::Compression:
    if (IsTextRole(role))
    {
      const QString method = QString::fromStdString(game.GetCompressionMethod());
      if (method.isEmpty())
        return tr("No Compression");
      return method;
    }
    return {};

  case Column::TimePlayed:
    return TimePlayedData(game, role);

  case Column::Tags:
    if (IsTextRole(role))
    {
      QStringList tags = GetGameTags(game.GetFilePath());
      tags.sort();
      return tags.join(QStringLiteral(", "));
    }
    return {};

  default:
    return {};
  }
}

QVariant GameListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return {};

  // The platform and region columns only show icons, so they have no header text.
  switch (static_cast<Column>(section))
  {
  case Column::Banner:
    return tr("Banner");
  case Column::Title:
    return tr("Title");
  case Column::Description:
    return tr("Description");
  case Column::Maker:
    return tr("Maker");
  case Column::ID:
    return tr("ID");
  case Column::Size:
    return tr("Size");
  case Column::FileName:
    return tr("File Name");
  case Column::FilePath:
    return tr("File Path");
  case Column::FileFormat:
    return tr("File Format");
  case Column::BlockSize:
    return tr("Block Size");
  case Column::Compression:
    return tr("Compression");
  case Column::TimePlayed:
    return tr("Time Played");
  case Column::Tags:
    return tr("Tags");
  default:
    return {};
  }
}

int GameListModel::rowCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : static_cast<int>(m_games.size());
}

int GameListModel::columnCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : static_cast<int>(Column::Count);
}

bool GameListModel::ShouldDisplayGameListItem(int index) const
{
  const UICommon::GameFile& game = *m_games[index];

  if (!m_search.isEmpty())
  {
    const QString title = QString::fromStdString(game.GetName(m_titles));
    const bool title_hit = title.contains(m_search, Qt::CaseInsensitive);

    // File names only count while the list view shows them.
    const bool file_name_shown = Config::Get(Config::MAIN_GAMELIST_COLUMN_FILE_NAME);
    const bool list_view = Settings::Instance().GetPreferredView();
    bool file_name_hit = false;
    if (file_name_shown && list_view)
    {
      file_name_hit =
          QString::fromStdString(game.GetFileName()).contains(m_search, Qt::CaseInsensitive);
    }

    if (!title_hit && !file_name_hit)
      return false;
  }

  bool known_platform = false;
  const Config::Info<bool>& platform_filter = PlatformFilter(game.GetPlatform(), &known_platform);
  if (!known_platform || !Config::Get(platform_filter))
    return false;

  return Config::Get(RegionFilter(game.GetCountry()));
}

std::shared_ptr<const UICommon::GameFile> GameListModel::GetGameFile(int index) const
{
  return m_games[index];
}

std::string GameListModel::GetNetPlayName(const UICommon::GameFile& game) const
{
  return game.GetNetPlayName(m_titles);
}

void GameListModel::AddGame(const std::shared_ptr<const UICommon::GameFile>& game)
{
  const int row = static_cast<int>(m_games.size());
  beginInsertRows(QModelIndex(), row, row);
  m_games.push_back(game);
  endInsertRows();
}

void GameListModel::UpdateGame(const std::shared_ptr<const UICommon::GameFile>& game)
{
  const int row = RowOf(game->GetFilePath());
  if (row < 0)
  {
    AddGame(game);
    return;
  }

  m_games[row] = game;
  emit dataChanged(createIndex(row, 0), createIndex(row, columnCount(QModelIndex()) - 1));
}

void GameListModel::RemoveGame(const std::string& path)
{
  const int row = RowOf(path);
  if (row < 0)
    return;

  beginRemoveRows(QModelIndex(), row, row);
  m_games.removeAt(row);
  endRemoveRows();
}

std::shared_ptr<const UICommon::GameFile> GameListModel::FindGame(const std::string& path) const
{
  const int row = RowOf(path);
  if (row < 0)
    return nullptr;
  return m_games[row];
}

int GameListModel::RowOf(const std::string& path) const
{
  const auto it = std::ranges::find_if(
      m_games, [&path](const auto& game) { return game->GetFilePath() == path; });
  return it == m_games.end() ? -1 : static_cast<int>(it - m_games.begin());
}

std::shared_ptr<const UICommon::GameFile>
GameListModel::FindSecondDisc(const UICommon::GameFile& game) const
{
  if (!DiscIO::IsDisc(game.GetPlatform()))
    return nullptr;

  // Another disc of the same game: preferably of the same revision, otherwise the last other
  // disc found.
  std::shared_ptr<const UICommon::GameFile> fallback;
  for (const auto& candidate : m_games)
  {
    if (candidate->GetGameID() != game.GetGameID() ||
        candidate->GetDiscNumber() == game.GetDiscNumber())
    {
      continue;
    }
    if (candidate->GetRevision() == game.GetRevision())
      return candidate;
    fallback = candidate;
  }
  return fallback;
}

void GameListModel::SetSearchTerm(const QString& term)
{
  m_search = term;
}

void GameListModel::SetScale(float scale)
{
  m_zoom = scale;
}

float GameListModel::GetScale() const
{
  return m_zoom;
}

const QStringList& GameListModel::GetAllTags() const
{
  return m_tags;
}

const QStringList GameListModel::GetGameTags(const std::string& path) const
{
  return m_tags_by_path.value(QString::fromStdString(path)).toStringList();
}

void GameListModel::SetTagsOf(const std::string& path, const QStringList& tags)
{
  m_tags_by_path[QString::fromStdString(path)] = tags;
  Settings::GetQSettings().setValue(GAME_TAGS_KEY, m_tags_by_path);

  const int row = RowOf(path);
  if (row < 0)
    return;
  const QModelIndex cell = createIndex(row, static_cast<int>(Column::Tags));
  emit dataChanged(cell, cell);
}

void GameListModel::StoreTagNames() const
{
  Settings::GetQSettings().setValue(TAGS_KEY, m_tags);
}

void GameListModel::AddGameTag(const std::string& path, const QString& name)
{
  QStringList tags = GetGameTags(path);
  if (tags.contains(name))
    return;

  tags.append(name);
  SetTagsOf(path, tags);
}

void GameListModel::RemoveGameTag(const std::string& path, const QString& name)
{
  QStringList tags = GetGameTags(path);
  tags.removeAll(name);
  SetTagsOf(path, tags);
}

void GameListModel::NewTag(const QString& name)
{
  if (m_tags.contains(name))
    return;

  m_tags.append(name);
  StoreTagNames();
}

void GameListModel::DeleteTag(const QString& name)
{
  m_tags.removeAll(name);

  const QList<QString> tagged_paths = m_tags_by_path.keys();
  for (const QString& path : tagged_paths)
    RemoveGameTag(path.toStdString(), name);

  StoreTagNames();
}

void GameListModel::PurgeCache()
{
  m_tracker.PurgeCache();
}
