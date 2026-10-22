// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/GCMemcardManager.h"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <QDialogButtonBox>
#include <QDir>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/VariantUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardUtils.h"

#include "DolphinQt/GCMemcardCreateNewDialog.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"

using ExpansionInterface::Slot;

namespace
{
enum SaveColumn : int
{
  COLUMN_FILENAME,
  COLUMN_BANNER,
  COLUMN_TITLE,
  COLUMN_ICON,
  COLUMN_BLOCKS,
  COLUMN_COUNT,
};

// Room for a 32 pixel icon or banner plus padding.
constexpr int ROW_HEIGHT = 36;
constexpr int ICON_PADDING = 6;

// Save icons change frames after 4, 8 or 12 frames at 60 Hz, so ticking at 15 Hz is enough.
constexpr int ANIMATION_TICKS_PER_SECOND = 15;

// Every cell of a save's row holds the save's directory index under this role.
constexpr int ROLE_DIRECTORY_INDEX = Qt::UserRole;

Slot OtherSlot(Slot slot)
{
  return slot == Slot::A ? Slot::B : Slot::A;
}

QString UserCardFolder()
{
  return QString::fromStdString(File::GetUserPath(D_GCUSER_IDX));
}

QString FormatName(Memcard::SavefileFormat format)
{
  using Format = Memcard::SavefileFormat;
  if (format == Format::GCS)
    return QObject::tr("MadCatz Gameshark files");
  if (format == Format::SAV)
    return QObject::tr("Datel MaxDrive/Pro files");
  // Anything unexpected is reported as GCI after the assertion.
  ASSERT(format == Format::GCI);
  return QObject::tr("Native GCI File");
}

void SetUpSaveTable(QTableWidget* table)
{
  table->setTabKeyNavigation(false);
  table->setHorizontalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);
  table->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);
}

void SetUpSaveColumns(QTableWidget* table)
{
  table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSortingEnabled(true);

  QHeaderView* const header = table->horizontalHeader();
  header->setHighlightSections(false);
  header->setMinimumSectionSize(0);
  header->setSortIndicatorShown(true);

  table->setColumnCount(COLUMN_COUNT);
  const QString titles[COLUMN_COUNT] = {GCMemcardManager::tr("Filename"),
                                        GCMemcardManager::tr("Banner"),
                                        GCMemcardManager::tr("Title"), GCMemcardManager::tr("Icon"),
                                        GCMemcardManager::tr("Blocks")};
  for (int column = 0; column < COLUMN_COUNT; ++column)
    table->setHorizontalHeaderItem(column, new QTableWidgetItem(titles[column]));

  const int widths[COLUMN_COUNT] = {100, Memcard::MEMORY_CARD_BANNER_WIDTH + ICON_PADDING, 160,
                                    Memcard::MEMORY_CARD_ICON_WIDTH + ICON_PADDING, 40};
  for (int column = 0; column < COLUMN_COUNT; ++column)
    table->setColumnWidth(column, widths[column]);

  table->verticalHeader()->setDefaultSectionSize(ROW_HEIGHT);
  table->verticalHeader()->hide();
  table->setShowGrid(false);
}
}  // namespace

struct GCMemcardManager::AnimatedIcon
{
  std::vector<QPixmap> frames;
  // One entry per animation tick: the index of the frame shown during that tick.
  std::vector<u8> schedule;
};

GCMemcardManager::GCMemcardManager(QWidget* parent) : QDialog(parent)
{
  BuildWidgets();
  ConnectWidgets();

  SelectSlot(Slot::A);
  UpdateButtons();

  m_animation_timer = new QTimer(this);
  connect(m_animation_timer, &QTimer::timeout, this, &GCMemcardManager::AnimateIcons);
  m_animation_timer->start(1000 / ANIMATION_TICKS_PER_SECOND);

  OpenConfiguredCards();

  resize(650, 500);
  setWindowTitle(tr("GameCube Memory Card Manager"));
}

GCMemcardManager::~GCMemcardManager() = default;

void GCMemcardManager::BuildWidgets()
{
  m_close_box = new QDialogButtonBox(QDialogButtonBox::Close);

  // The texts of the first two depend on the active slot (see SelectSlot).
  m_switch_button = new NonDefaultQPushButton;
  m_copy_button = new NonDefaultQPushButton;
  m_delete_button = new NonDefaultQPushButton(tr("&Delete"));

  // Export is a split button: the main part exports as .gci, the arrow offers the other formats.
  m_export_button = new QToolButton(this);
  m_export_menu = new QMenu(m_export_button);
  m_export_gci = new QAction(tr("&Export as .gci..."), m_export_menu);
  m_export_gcs = new QAction(tr("Export as .&gcs..."), m_export_menu);
  m_export_sav = new QAction(tr("Export as .&sav..."), m_export_menu);
  for (QAction* const action : {m_export_gci, m_export_gcs, m_export_sav})
    m_export_menu->addAction(action);
  m_export_button->setDefaultAction(m_export_gci);
  m_export_button->setPopupMode(QToolButton::MenuButtonPopup);
  m_export_button->setMenu(m_export_menu);

  m_import_button = new NonDefaultQPushButton(tr("&Import..."));
  m_fix_button = new NonDefaultQPushButton(tr("Fix Checksums"));

  auto* const grid = new QGridLayout;

  for (const Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    auto* const group = new QGroupBox(slot == Slot::A ? tr("Slot A") : tr("Slot B"));
    m_groups[slot] = group;
    m_paths[slot] = new QLineEdit;
    m_open_buttons[slot] = new NonDefaultQPushButton(tr("&Open..."));
    m_create_buttons[slot] = new NonDefaultQPushButton(tr("&Create..."));
    m_tables[slot] = new QTableWidget;
    SetUpSaveTable(m_tables[slot]);
    m_free_space[slot] = new QLabel;
    SetUpSaveColumns(m_tables[slot]);

    // Path, Open and Create on top, the saves below, the free space at the bottom.
    auto* const slot_grid = new QGridLayout;
    group->setLayout(slot_grid);
    slot_grid->addWidget(m_paths[slot], 0, 0);
    slot_grid->addWidget(m_open_buttons[slot], 0, 1);
    slot_grid->addWidget(m_create_buttons[slot], 0, 2);
    slot_grid->addWidget(m_tables[slot], 1, 0, 1, 3);
    slot_grid->addWidget(m_free_space[slot], 2, 0);

    // Slot A on the left, slot B on the right, the buttons in the middle column.
    grid->addWidget(group, 0, slot == Slot::A ? 0 : 2, 8, 1);

    FillTable(slot);
  }

  int row = 1;
  for (QWidget* const button :
       std::initializer_list<QWidget*>{m_switch_button, m_copy_button, m_delete_button,
                                       m_export_button, m_import_button, m_fix_button})
  {
    grid->addWidget(button, row++, 1);
  }
  grid->addWidget(m_close_box, 8, 2);

  setLayout(grid);
}

void GCMemcardManager::ConnectWidgets()
{
  connect(m_close_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_switch_button, &QPushButton::clicked,
          [this] { SelectSlot(OtherSlot(m_active_slot)); });
  connect(m_export_gci, &QAction::triggered,
          [this] { ExportSaves(Memcard::SavefileFormat::GCI); });
  connect(m_export_gcs, &QAction::triggered,
          [this] { ExportSaves(Memcard::SavefileFormat::GCS); });
  connect(m_export_sav, &QAction::triggered,
          [this] { ExportSaves(Memcard::SavefileFormat::SAV); });
  connect(m_delete_button, &QPushButton::clicked, this, &GCMemcardManager::DeleteSaves);
  connect(m_import_button, &QPushButton::clicked, this, &GCMemcardManager::ImportFromFiles);
  connect(m_copy_button, &QPushButton::clicked, this, &GCMemcardManager::CopyToOtherSlot);
  connect(m_fix_button, &QPushButton::clicked, this, &GCMemcardManager::FixChecksums);

  for (const Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    // Typing or picking a path opens the card right away.
    connect(m_paths[slot], &QLineEdit::textChanged,
            [this, slot](const QString& path) { OpenCard(slot, path); });
    connect(m_open_buttons[slot], &QPushButton::clicked, [this, slot] { BrowseForCard(slot); });
    connect(m_create_buttons[slot], &QPushButton::clicked, [this, slot] { CreateCard(slot); });
    connect(m_tables[slot], &QTableWidget::itemSelectionChanged, this,
            &GCMemcardManager::UpdateButtons);
  }
}

void GCMemcardManager::OpenConfiguredCards()
{
  // Start with the cards the emulated slots use, where they hold a memory card image.
  for (const Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    if (Config::Get(Config::GetInfoForEXIDevice(slot)) !=
        ExpansionInterface::EXIDeviceType::MemoryCard)
    {
      continue;
    }

    const std::string path =
        Config::GetMemcardPath(slot, Config::Get(Config::MAIN_FALLBACK_REGION));
    OpenCard(slot, QString::fromStdString(path));
  }
}

void GCMemcardManager::SelectSlot(Slot slot)
{
  // Only the active slot's table can be used; the buttons act on it.
  for (const Slot other : ExpansionInterface::MEMCARD_SLOTS)
    m_tables[other]->setEnabled(other == slot);

  const bool a = slot == Slot::A;
  m_switch_button->setText(a ? tr("Switch to B") : tr("Switch to A"));
  m_copy_button->setText(a ? tr("Copy to B") : tr("Copy to A"));

  m_active_slot = slot;
  UpdateButtons();
}

void GCMemcardManager::FillTable(Slot slot)
{
  m_icons[slot].clear();

  QTableWidget* const table = m_tables[slot];
  if (!m_cards[slot])
  {
    table->setRowCount(0);
    m_free_space[slot]->clear();
    return;
  }

  Memcard::GCMemcard& card = *m_cards[slot];

  // Rows are added in directory order; sorting is off meanwhile so they stay where put.
  table->setSortingEnabled(false);

  const u8 save_count = card.GetNumFiles();
  const u8 free_entries = Memcard::DIRLEN - save_count;
  const u16 free_blocks = card.GetFreeBlocks();
  table->setRowCount(save_count);

  for (int row = 0; row < save_count; ++row)
  {
    const u8 index = card.GetFileIndex(static_cast<u8>(row));

    const auto comments = card.GetSaveComments(index);
    const u16 blocks = card.DEntry_BlockCount(index);
    const auto entry = card.GetDEntry(index);
    const std::string file_name = entry ? Memcard::GenerateFilename(*entry) : "";

    QString title;
    QString comment;
    if (comments)
    {
      title = QString::fromStdString(comments->first).trimmed();
      comment = QString::fromStdString(comments->second).trimmed();
    }

    QPixmap banner = BannerOf(index, slot);
    AnimatedIcon icon = IconOf(index, slot);

    QTableWidgetItem* const cells[COLUMN_COUNT] = {
        new QTableWidgetItem(QString::fromStdString(file_name)),
        new QTableWidgetItem(),
        new QTableWidgetItem(QStringLiteral("%1\n%2").arg(title, comment)),
        new QTableWidgetItem(),
        new QTableWidgetItem(),
    };
    cells[COLUMN_BANNER]->setData(Qt::DecorationRole, banner);
    cells[COLUMN_ICON]->setData(Qt::DecorationRole, icon.frames[0]);
    cells[COLUMN_BLOCKS]->setData(Qt::DisplayRole, blocks);

    for (QTableWidgetItem* const cell : cells)
    {
      cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      cell->setData(ROLE_DIRECTORY_INDEX, static_cast<int>(index));
    }

    m_icons[slot].emplace(index, std::move(icon));

    for (int column = 0; column < COLUMN_COUNT; ++column)
      table->setItem(row, column, cells[column]);
  }

  const QString blocks_text = tr("Free Blocks: %1").arg(free_blocks);
  const QString entries_text = tr("Free Files: %1").arg(free_entries);
  m_free_space[slot]->setText(QStringLiteral("%1      %2").arg(blocks_text, entries_text));

  table->setSortingEnabled(true);
}

void GCMemcardManager::UpdateButtons()
{
  const bool has_selection = !m_tables[m_active_slot]->selectedItems().isEmpty();
  const bool has_card = m_cards[m_active_slot] != nullptr;
  const bool other_has_card = m_cards[OtherSlot(m_active_slot)] != nullptr;

  m_copy_button->setEnabled(has_selection && other_has_card);
  m_export_button->setEnabled(has_selection);
  m_import_button->setEnabled(has_card);
  m_delete_button->setEnabled(has_selection);
  m_fix_button->setEnabled(has_card);
}

void GCMemcardManager::OpenCard(Slot slot, const QString& path)
{
  auto [error, card] = Memcard::GCMemcard::Open(path.toStdString());

  const bool usable = !error.HasCriticalErrors() && card && card->IsValid();
  if (usable)
  {
    // Note: this re-enters OpenCard through textChanged when the path text actually changes.
    m_paths[slot]->setText(path);
    m_cards[slot] = std::make_unique<Memcard::GCMemcard>(std::move(*card));
  }
  else
  {
    m_cards[slot] = nullptr;
    ModalMessageBox::warning(
        this, tr("Error"),
        tr("Failed opening memory card:\n%1").arg(GetErrorMessagesForErrorCode(error)));
  }

  FillTable(slot);
  UpdateButtons();
}

void GCMemcardManager::BrowseForCard(Slot slot)
{
  const QString caption =
      slot == Slot::A ? tr("Set Memory Card File for Slot A") : tr("Set Memory Card File for Slot B");
  const QString filter =
      QStringLiteral("%1 (*.raw *.gcp);;%2 (*)").arg(tr("GameCube Memory Cards"), tr("All Files"));
  const QString path = QDir::toNativeSeparators(
      DolphinFileDialog::getOpenFileName(this, caption, UserCardFolder(), filter));
  if (!path.isEmpty())
    m_paths[slot]->setText(path);
}

std::vector<u8> GCMemcardManager::SelectedSaves()
{
  // Selected cells, folded into a set of directory indices (ascending).
  std::vector<bool> picked(Memcard::DIRLEN);
  for (const QTableWidgetItem* const cell : m_tables[m_active_slot]->selectedItems())
  {
    const int index = cell->data(ROLE_DIRECTORY_INDEX).toInt();
    if (index < 0 || index >= static_cast<int>(Memcard::DIRLEN))
    {
      ModalMessageBox::warning(this, tr("Error"),
                               tr("Data inconsistency in GCMemcardManager, aborting action."));
      return {};
    }
    picked[static_cast<size_t>(index)] = true;
  }

  std::vector<u8> indices;
  for (size_t index = 0; index < picked.size(); ++index)
  {
    if (picked[index])
      indices.push_back(static_cast<u8>(index));
  }
  return indices;
}

void GCMemcardManager::ExportSaves(Memcard::SavefileFormat format)
{
  const auto& card = m_cards[m_active_slot];
  if (!card)
    return;

  const std::vector<u8> selected = SelectedSaves();
  if (selected.empty())
    return;

  const auto saves = Memcard::GetSavefiles(*card, selected);
  if (saves.empty())
  {
    ModalMessageBox::warning(this, tr("Export Failed"),
                             tr("Failed to read selected savefile(s) from memory card."));
    return;
  }

  const std::string extension = Memcard::GetDefaultExtension(format);

  // A single save goes exactly where the user says.
  if (saves.size() == 1)
  {
    const std::string suggestion =
        fmt::format("{}/{}{}", File::GetUserPath(D_GCUSER_IDX),
                    Memcard::GenerateFilename(saves[0].dir_entry), extension);
    const QString filter = QStringLiteral("%1 (*%2);;%3 (*)")
                               .arg(FormatName(format), QString::fromStdString(extension),
                                    tr("All Files"));
    const QString chosen = DolphinFileDialog::getSaveFileName(
        this, tr("Export Save File"), QString::fromStdString(suggestion), filter);
    if (chosen.isEmpty())
      return;

    const std::string path = chosen.toStdString();
    if (!Memcard::WriteSavefile(path, saves[0], format))
    {
      File::Delete(path);
      ModalMessageBox::warning(this, tr("Export Failed"), tr("Failed to write savefile to disk."));
    }
    return;
  }

  // Several saves go into a folder, each under its own name, never overwriting a file.
  const QString chosen_dir = DolphinFileDialog::getExistingDirectory(
      this, QObject::tr("Export Save Files"), UserCardFolder());
  if (chosen_dir.isEmpty())
    return;

  const std::string dir = chosen_dir.toStdString();
  size_t failed = 0;
  for (const auto& save : saves)
  {
    const std::string stem = dir + DIR_SEP + Memcard::GenerateFilename(save.dir_entry);
    std::string path = stem + extension;
    for (size_t n = 0; File::Exists(path); ++n)
      path = fmt::format("{}_{}{}", stem, n, extension);

    if (!Memcard::WriteSavefile(path, save, format))
    {
      File::Delete(path);
      ++failed;
    }
  }

  if (failed == 0)
    return;

  const QString failed_text =
      tr("Failed to export %n out of %1 save file(s).", "", static_cast<int>(failed))
          .arg(saves.size());
  if (failed == saves.size())
  {
    ModalMessageBox::warning(this, tr("Export Failed"), failed_text);
    return;
  }

  const QString done_text = tr("Successfully exported %n out of %1 save file(s).", "",
                               static_cast<int>(saves.size() - failed))
                                .arg(saves.size());
  ModalMessageBox::warning(this, tr("Export Failed"),
                           QStringLiteral("%1\n%2").arg(failed_text, done_text));
}

void GCMemcardManager::ImportInto(Slot slot, std::span<const Memcard::Savefile> saves)
{
  auto& card = m_cards[slot];
  if (!card)
    return;

  // Check everything that can be checked up front, and report all problems at once.
  const size_t needed_entries = saves.size();
  const size_t needed_blocks = Memcard::GetBlockCount(saves);
  const size_t free_entries = Memcard::DIRLEN - card->GetNumFiles();
  const size_t free_blocks = card->GetFreeBlocks();

  QStringList problems;
  if (needed_entries > free_entries)
  {
    problems.push_back(
        tr("Not enough free files on the target memory card. At least %n free file(s) required.",
           "", static_cast<int>(needed_entries)));
  }
  if (needed_blocks > free_blocks)
  {
    problems.push_back(
        tr("Not enough free blocks on the target memory card. At least %n free block(s) required.",
           "", static_cast<int>(needed_blocks)));
  }
  if (Memcard::HasDuplicateIdentity(saves))
  {
    problems.push_back(
        tr("At least two of the selected save files have the same internal filename."));
  }
  for (const Memcard::Savefile& save : saves)
  {
    if (!card->TitlePresent(save.dir_entry))
      continue;
    problems.push_back(tr("The target memory card already contains a file \"%1\".")
                           .arg(QString::fromStdString(Memcard::GenerateFilename(save.dir_entry))));
  }

  if (!problems.empty())
  {
    ModalMessageBox::warning(this, tr("Import Failed"), problems.join(QLatin1Char('\n')));
    return;
  }

  for (const Memcard::Savefile& save : saves)
  {
    // After the checks above, this only fails on a damaged card.
    if (card->ImportFile(save) == Memcard::GCMemcardImportFileRetVal::SUCCESS)
      continue;

    ModalMessageBox::warning(this, tr("Import Failed"),
                             tr("Failed to import \"%1\".")
                                 .arg(QString::fromStdString(
                                     Memcard::GenerateFilename(save.dir_entry))));
    break;
  }

  if (!card->Save())
  {
    ModalMessageBox::warning(this, tr("Import Failed"),
                             tr("Failed to write modified memory card to disk."));
  }

  FillTable(slot);
}

void GCMemcardManager::ImportFromFiles()
{
  if (!m_cards[m_active_slot])
    return;

  const QString filter =
      QStringLiteral("%1 (*.gci *.gcs *.sav);;%2 (*.gci);;%3 (*.gcs);;%4 (*.sav);;%5 (*)")
          .arg(tr("Supported file formats"), FormatName(Memcard::SavefileFormat::GCI),
               FormatName(Memcard::SavefileFormat::GCS),
               FormatName(Memcard::SavefileFormat::SAV), tr("All Files"));
  const QStringList paths = DolphinFileDialog::getOpenFileNames(this, tr("Import Save File(s)"),
                                                                UserCardFolder(), filter);
  if (paths.isEmpty())
    return;

  // Read every file first; any unreadable one cancels the whole import.
  std::vector<Memcard::Savefile> saves;
  saves.reserve(static_cast<size_t>(paths.size()));
  QStringList errors;
  for (const QString& path : paths)
  {
    std::visit(overloaded{
                   [&saves](Memcard::Savefile save) { saves.emplace_back(std::move(save)); },
                   [&errors, &path](Memcard::ReadSavefileErrorCode error) {
                     errors.push_back(tr("%1: %2").arg(path, GetErrorMessageForErrorCode(error)));
                   },
               },
               Memcard::ReadSavefile(path.toStdString()));
  }

  if (!errors.empty())
  {
    ModalMessageBox::warning(
        this, tr("Import Failed"),
        tr("Encountered the following errors while opening save files:\n%1\n\nAborting import.")
            .arg(errors.join(QStringLiteral("\n"))));
    return;
  }

  ImportInto(m_active_slot, saves);
}

void GCMemcardManager::CopyToOtherSlot()
{
  const auto& source = m_cards[m_active_slot];
  if (!source || !m_cards[OtherSlot(m_active_slot)])
    return;

  const std::vector<u8> selected = SelectedSaves();
  if (selected.empty())
    return;

  const auto saves = Memcard::GetSavefiles(*source, selected);
  if (saves.empty())
  {
    ModalMessageBox::warning(this, tr("Copy Failed"),
                             tr("Failed to read selected savefile(s) from memory card."));
    return;
  }

  ImportInto(OtherSlot(m_active_slot), saves);
}

void GCMemcardManager::DeleteSaves()
{
  auto& card = m_cards[m_active_slot];
  if (!card)
    return;

  const std::vector<u8> selected = SelectedSaves();
  if (selected.empty())
    return;

  const QString question = tr("Do you want to delete the %n selected save file(s)?", "",
                              static_cast<int>(selected.size()));
  if (ModalMessageBox::question(this, tr("Question"), question) != QMessageBox::Yes)
    return;

  for (const u8 index : selected)
  {
    if (card->RemoveFile(index) == Memcard::GCMemcardRemoveFileRetVal::SUCCESS)
      continue;
    ModalMessageBox::warning(this, tr("Remove Failed"), tr("Failed to remove file."));
    break;
  }

  if (!card->Save())
  {
    ModalMessageBox::warning(this, tr("Remove Failed"),
                             tr("Failed to write modified memory card to disk."));
  }

  FillTable(m_active_slot);
  UpdateButtons();
}

void GCMemcardManager::FixChecksums()
{
  // Only reachable with a card open (the button is disabled otherwise).
  Memcard::GCMemcard& card = *m_cards[m_active_slot];
  card.FixChecksums();

  if (!card.Save())
  {
    ModalMessageBox::warning(this, tr("Fix Checksums Failed"),
                             tr("Failed to write modified memory card to disk."));
  }
}

void GCMemcardManager::CreateCard(Slot slot)
{
  GCMemcardCreateNewDialog dialog(this);
  if (dialog.exec() == QDialog::Accepted)
    m_paths[slot]->setText(QString::fromStdString(dialog.GetMemoryCardPath()));
}

void GCMemcardManager::AnimateIcons()
{
  for (const Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    QTableWidget* const table = m_tables[slot];
    const int rows = table->rowCount();
    if (rows <= 0)
      continue;

    // Only the rows inside the viewport are touched.
    const QRect area = table->viewport()->rect();
    const int top_row = table->indexAt(area.topLeft()).row();
    if (top_row >= rows)
      continue;
    const int bottom_row = table->indexAt(area.bottomLeft()).row();
    const int first = std::max(top_row, 0);
    const int last = bottom_row < 0 ? rows - 1 : std::min(bottom_row, rows - 1);

    for (int row = first; row <= last; ++row)
    {
      QTableWidgetItem* const cell = table->item(row, COLUMN_ICON);
      if (!cell)
        continue;

      const auto index = static_cast<u8>(cell->data(ROLE_DIRECTORY_INDEX).toInt());
      const auto found = m_icons[slot].find(index);
      if (found == m_icons[slot].end())
        continue;

      const AnimatedIcon& icon = found->second;
      if (icon.frames.size() <= 1 || icon.schedule.empty())
        continue;

      // Only touch the cell when the frame actually changes from the last tick.
      const u64 length = icon.schedule.size();
      const u8 before = icon.schedule[(m_tick - 1) % length];
      const u8 now = icon.schedule[m_tick % length];
      if (before != now)
        cell->setData(Qt::DecorationRole, icon.frames[now]);
    }
  }

  ++m_tick;
}

QPixmap GCMemcardManager::BannerOf(u8 file_index, Slot slot)
{
  const auto pixels = m_cards[slot]->ReadBannerRGBA8(file_index);
  if (!pixels)
    return QPixmap::fromImage(QImage());

  const QImage image(reinterpret_cast<const u8*>(pixels->data()),
                     Memcard::MEMORY_CARD_BANNER_WIDTH, Memcard::MEMORY_CARD_BANNER_HEIGHT,
                     QImage::Format_ARGB32);
  return QPixmap::fromImage(image);
}

GCMemcardManager::AnimatedIcon GCMemcardManager::IconOf(u8 file_index, Slot slot)
{
  Memcard::GCMemcard& card = *m_cards[slot];
  AnimatedIcon icon;

  const auto frames = card.ReadAnimRGBA8(file_index);
  if (!frames || frames->empty())
  {
    // No icon: a single empty frame.
    icon.frames.emplace_back();
    icon.schedule.push_back(0);
    return icon;
  }

  const auto show_for = [&icon, &frames](size_t frame) {
    for (int tick = 0; tick < (*frames)[frame].delay; ++tick)
      icon.schedule.push_back(static_cast<u8>(frame));
  };

  icon.frames.reserve(frames->size());
  for (size_t frame = 0; frame < frames->size(); ++frame)
  {
    const QImage image(reinterpret_cast<const u8*>((*frames)[frame].image_data.data()),
                       Memcard::MEMORY_CARD_ICON_WIDTH, Memcard::MEMORY_CARD_ICON_HEIGHT,
                       QImage::Format_ARGB32);
    icon.frames.push_back(QPixmap::fromImage(image));
    show_for(frame);
  }

  // A "ping-pong" animation plays forwards, then backwards without repeating the end frames:
  // a b c d c b, a b c d c b, ...
  if (card.DEntry_IsPingPong(file_index) && frames->size() >= 3)
  {
    for (size_t frame = frames->size() - 2; frame > 0; --frame)
      show_for(frame);
  }

  return icon;
}

QString GCMemcardManager::GetErrorMessagesForErrorCode(const Memcard::GCMemcardErrorCode& code)
{
  using Issue = Memcard::GCMemcardValidityIssues;

  QStringList messages;
  const auto check = [&](Issue issue, const QString& message) {
    if (code.Test(issue))
      messages.push_back(message);
  };

  check(Issue::FAILED_TO_OPEN, tr("Couldn't open file."));
  check(Issue::IO_ERROR, tr("Couldn't read file."));
  check(Issue::INVALID_CARD_SIZE,
        tr("Filesize does not match any known GameCube Memory Card size."));
  check(Issue::MISMATCHED_CARD_SIZE, tr("Filesize in header mismatches actual card size."));
  check(Issue::INVALID_CHECKSUM, tr("Invalid checksums."));
  check(Issue::FREE_BLOCK_MISMATCH,
        tr("Mismatch between free block count in header and actually unused blocks."));
  check(Issue::DIR_BAT_INCONSISTENT, tr("Mismatch between internal data structures."));
  check(Issue::DATA_IN_UNUSED_AREA, tr("Data in area of file that should be unused."));

  if (messages.empty())
    return tr("No errors.");
  return messages.join(QLatin1Char{'\n'});
}

QString GCMemcardManager::GetErrorMessageForErrorCode(Memcard::ReadSavefileErrorCode code)
{
  using Failure = Memcard::ReadSavefileErrorCode;
  if (code == Failure::OpenFileFail)
    return tr("Failed to open file.");
  if (code == Failure::IOError)
    return tr("Failed to read from file.");
  if (code == Failure::DataCorrupted)
    return tr("Data in unrecognized format or corrupted.");
  return tr("Unknown error.");
}
