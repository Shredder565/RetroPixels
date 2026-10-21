// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/ResourcePackManager.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>

#include "Common/FileUtil.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/ResourcePack.h"

namespace
{
enum Column : int
{
  LOGO,
  NAME,
  VERSION,
  DESCRIPTION,
  AUTHOR,
  WEBSITE,
  COLUMN_COUNT
};

QString ToQString(const std::optional<std::string>& text)
{
  return QString::fromStdString(text.value_or(""));
}

QIcon LogoOf(const ResourcePack::ResourcePack& pack)
{
  const std::vector<char>& bytes = pack.GetLogo();
  QPixmap logo;
  logo.loadFromData(reinterpret_cast<const uchar*>(bytes.data()), static_cast<int>(bytes.size()));
  return QIcon(logo);
}
}  // namespace

ResourcePackManager::ResourcePackManager(QWidget* parent) : QDialog(parent)
{
  BuildLayout();
  WireUp();
  FillTable();

  setWindowTitle(tr("Resource Pack Manager"));
  resize(QSize(900, 600));
}

void ResourcePackManager::BuildLayout()
{
  m_packs = new QTableWidget;
  m_packs->setTabKeyNavigation(false);

  m_folder_button = new NonDefaultQPushButton(tr("Open Directory..."));
  m_toggle_button = new NonDefaultQPushButton(tr("Install"));
  m_delete_button = new NonDefaultQPushButton(tr("Remove"));
  m_rescan_button = new NonDefaultQPushButton(tr("Refresh"));
  m_up_button = new NonDefaultQPushButton(tr("Up"));
  m_down_button = new NonDefaultQPushButton(tr("Down"));

  auto* const close_box = new QDialogButtonBox(QDialogButtonBox::Ok);
  connect(close_box, &QDialogButtonBox::accepted, this, &QDialog::accept);

  // The table fills the left; the action buttons stack down the right in a single column.
  auto* const grid = new QGridLayout;
  grid->addWidget(m_packs, 0, 0, 7, 1);
  const std::array side_buttons{m_folder_button, m_toggle_button, m_delete_button,
                                m_rescan_button, m_up_button,     m_down_button};
  int grid_row = 0;
  for (QPushButton* const button : side_buttons)
    grid->addWidget(button, grid_row++, 1);
  grid->addWidget(close_box, 7, 1, Qt::AlignRight);

  // Applied twice, as it always has been; the second call only makes Qt print a warning.
  setLayout(grid);
  setLayout(grid);
}

void ResourcePackManager::WireUp()
{
  connect(m_folder_button, &QPushButton::clicked, this, &ResourcePackManager::ShowPackFolder);
  connect(m_rescan_button, &QPushButton::clicked, this, &ResourcePackManager::Rescan);
  connect(m_toggle_button, &QPushButton::clicked, this, &ResourcePackManager::ToggleInstalled);
  connect(m_delete_button, &QPushButton::clicked, this, &ResourcePackManager::DeleteSelected);
  connect(m_up_button, &QPushButton::clicked, this, &ResourcePackManager::MoveSelectedUp);
  connect(m_down_button, &QPushButton::clicked, this, &ResourcePackManager::MoveSelectedDown);

  connect(m_packs, &QTableWidget::itemSelectionChanged, this, &ResourcePackManager::UpdateButtons);
  connect(m_packs, &QTableWidget::itemDoubleClicked, this, &ResourcePackManager::FollowLink);
}

void ResourcePackManager::ShowPackFolder()
{
  const QString folder = QString::fromStdString(File::GetUserPath(D_RESOURCEPACK_IDX));
  QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void ResourcePackManager::FillTable()
{
  m_packs->clear();
  m_packs->setColumnCount(COLUMN_COUNT);
  m_packs->setHorizontalHeaderLabels(
      {QString{}, tr("Name"), tr("Version"), tr("Description"), tr("Author"), tr("Website")});

  QHeaderView* const header = m_packs->horizontalHeader();
  for (const int column : {LOGO, NAME, VERSION, DESCRIPTION})
    header->setSectionResizeMode(column, QHeaderView::ResizeToContents);
  header->setStretchLastSection(true);
  header->setHighlightSections(false);

  std::vector<ResourcePack::ResourcePack>& packs = ResourcePack::GetPacks();
  const int pack_count = static_cast<int>(packs.size());

  m_packs->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_packs->setSelectionMode(QAbstractItemView::SingleSelection);
  m_packs->setRowCount(pack_count);
  m_packs->setIconSize(QSize(32, 32));

  for (int row = 0; row < pack_count; ++row)
  {
    const ResourcePack::ResourcePack& pack = packs[static_cast<size_t>(PackIndexOfRow(row))];
    const ResourcePack::Manifest& manifest = *pack.GetManifest();

    std::array<QTableWidgetItem*, COLUMN_COUNT> cells{};
    cells[LOGO] = new QTableWidgetItem;
    cells[LOGO]->setIcon(LogoOf(pack));
    cells[NAME] = new QTableWidgetItem(QString::fromStdString(manifest.GetName()));
    cells[VERSION] = new QTableWidgetItem(QString::fromStdString(manifest.GetVersion()));
    cells[DESCRIPTION] = new QTableWidgetItem(ToQString(manifest.GetDescription()));
    cells[AUTHOR] = new QTableWidgetItem(manifest.GetAuthors() ?
                                             QString::fromStdString(*manifest.GetAuthors()) :
                                             tr("Unknown author"));

    // The website looks like a link, and double-clicking it opens the URL kept in UserRole.
    const QString url = ToQString(manifest.GetWebsite());
    cells[WEBSITE] = new QTableWidgetItem(url);
    QFont underlined = cells[WEBSITE]->font();
    underlined.setUnderline(true);
    cells[WEBSITE]->setFont(underlined);
    cells[WEBSITE]->setForeground(QBrush(Qt::blue));
    cells[WEBSITE]->setData(Qt::UserRole, url);

    // Installed packs stand out in bold on green.
    const bool installed = ResourcePack::IsInstalled(pack);
    for (QTableWidgetItem* const cell : cells)
    {
      cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      if (!installed)
        continue;
      cell->setBackground(QColor(Qt::green));
      QFont bold = cell->font();
      bold.setBold(true);
      cell->setFont(bold);
    }

    for (int column = 0; column < COLUMN_COUNT; ++column)
      m_packs->setItem(row, column, cells[static_cast<size_t>(column)]);
  }

  UpdateButtons();
}

int ResourcePackManager::PackIndexOfRow(int row) const
{
  return m_packs->rowCount() - 1 - row;
}

ResourcePack::ResourcePack* ResourcePackManager::SelectedPack() const
{
  const QList<QTableWidgetItem*> selection = m_packs->selectedItems();
  if (selection.empty())
    return nullptr;
  const int index = PackIndexOfRow(selection.front()->row());
  return &ResourcePack::GetPacks()[static_cast<size_t>(index)];
}

void ResourcePackManager::ToggleInstalled()
{
  const ResourcePack::ResourcePack* const pack = SelectedPack();
  if (pack == nullptr)
    return;

  if (ResourcePack::IsInstalled(*pack))
    UninstallSelected();
  else
    InstallSelected();
}

void ResourcePackManager::InstallSelected()
{
  ResourcePack::ResourcePack* const pack = SelectedPack();
  if (pack == nullptr)
    return;

  if (!pack->Install(File::GetUserPath(D_LOAD_IDX)))
  {
    const QString why = QString::fromStdString(pack->GetError());
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to install pack: %1").arg(why));
  }
  FillTable();
}

void ResourcePackManager::UninstallSelected()
{
  ResourcePack::ResourcePack* const pack = SelectedPack();
  if (pack == nullptr)
    return;

  if (!pack->Uninstall(File::GetUserPath(D_LOAD_IDX)))
  {
    const QString why = QString::fromStdString(pack->GetError());
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to uninstall pack: %1").arg(why));
  }
  FillTable();
}

void ResourcePackManager::DeleteSelected()
{
  if (m_packs->selectedItems().empty())
    return;

  ModalMessageBox confirm(this);
  confirm.setWindowTitle(tr("Confirmation"));
  confirm.setText(tr("Are you sure you want to delete this pack?"));
  confirm.setIcon(QMessageBox::Warning);
  confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Abort);
  if (confirm.exec() != QMessageBox::Yes)
    return;

  // Uninstalling first takes its textures out of the load folder; it also refills the table,
  // which is why the pack is looked up before that.
  ResourcePack::ResourcePack& doomed = *SelectedPack();
  const std::string file = doomed.GetPath();
  UninstallSelected();
  ResourcePack::Remove(doomed);
  File::Delete(file);
  FillTable();
}

// Moving a pack re-adds it at a neighbouring index, then selects the row at that index's number.
// The first and last rows are special-cased the same way the buttons always have been.
void ResourcePackManager::MoveSelectedDown()
{
  const QList<QTableWidgetItem*> selection = m_packs->selectedItems();
  if (selection.empty())
    return;

  const int row = selection.front()->row();
  if (row >= m_packs->rowCount())
    return;

  int index = PackIndexOfRow(row);
  ResourcePack::ResourcePack& pack = ResourcePack::GetPacks()[static_cast<size_t>(index)];
  const std::string file = pack.GetPath();
  index -= 1;

  ResourcePack::Remove(pack);
  ResourcePack::Add(file, index);
  FillTable();

  m_packs->selectRow(index == 0 ? m_packs->rowCount() - 1 : index);
}

void ResourcePackManager::MoveSelectedUp()
{
  const QList<QTableWidgetItem*> selection = m_packs->selectedItems();
  if (selection.empty())
    return;

  const int row = selection.front()->row();
  if (row == 0)
    return;

  int index = PackIndexOfRow(row);
  ResourcePack::ResourcePack& pack = ResourcePack::GetPacks()[static_cast<size_t>(index)];
  const std::string file = pack.GetPath();
  index += 1;

  const bool past_end = row == m_packs->rowCount();
  ResourcePack::Remove(pack);
  ResourcePack::Add(file, past_end ? -1 : index);
  FillTable();

  m_packs->selectRow(index == m_packs->rowCount() - 1 ? 0 : index);
}

void ResourcePackManager::Rescan()
{
  ResourcePack::Init();
  FillTable();
}

void ResourcePackManager::UpdateButtons()
{
  const QList<QTableWidgetItem*> selection = m_packs->selectedItems();
  const bool any = !selection.empty();

  if (any)
  {
    const auto& pack =
        ResourcePack::GetPacks()[static_cast<size_t>(PackIndexOfRow(selection.front()->row()))];
    m_toggle_button->setText(ResourcePack::IsInstalled(pack) ? tr("Uninstall") : tr("Install"));
  }

  m_toggle_button->setEnabled(any);
  m_delete_button->setEnabled(any);

  const int row = any ? selection.front()->row() : -1;
  m_down_button->setEnabled(any && row < m_packs->rowCount() - 1);
  m_up_button->setEnabled(any && row != 0);
}

void ResourcePackManager::FollowLink(QTableWidgetItem* item)
{
  const QVariant url = item->data(Qt::UserRole);
  if (!url.isNull())
    QDesktopServices::openUrl(QUrl(url.toString()));
}
