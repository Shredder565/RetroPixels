// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/FilesystemWidget.h"

#include <future>
#include <optional>

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QMenu>
#include <QStandardItemModel>
#include <QStyleFactory>
#include <QTreeView>

#include "Common/StringUtil.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "DolphinQt/Resources.h"
#include "UICommon/UICommon.h"

namespace
{
// Item data roles in the first column.
constexpr int ROLE_PARTITION = Qt::UserRole;      // index into GetPartitions(), or -1
constexpr int ROLE_PATH = Qt::UserRole + 1;       // path inside the partition
constexpr int ROLE_KIND = Qt::UserRole + 2;       // NodeKind

enum class NodeKind
{
  Disc = -2,
  Partition = -1,
  File = 0,
  Folder = 1,
};
}  // namespace

Q_DECLARE_METATYPE(NodeKind);

FilesystemWidget::FilesystemWidget(std::shared_ptr<DiscIO::Volume> volume)
    : m_volume(std::move(volume))
{
  // A grid rather than a box layout: with some styles (KDE Plasma) a tree
  // alone in a QVBoxLayout is drawn with square corners.
  auto* const grid = new QGridLayout{this};

  m_model = new QStandardItemModel(0, 2);
  m_model->setHorizontalHeaderLabels({tr("Name"), tr("Size")});

  m_tree = new QTreeView(this);
  m_tree->setModel(m_model);
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  m_model->setParent(m_tree);

  QHeaderView* const header = m_tree->header();
  header->setSectionResizeMode(0, QHeaderView::Stretch);
  header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  header->setStretchLastSection(false);
  header->setDefaultAlignment(Qt::AlignCenter);

#ifdef _WIN32
  // The Vista style draws no branch lines; the classic Windows style does.
  if (QApplication::style()->objectName() == QStringLiteral("windowsvista"))
    m_tree->setStyle(QStyleFactory::create(QStringLiteral("windows")));
#endif

  grid->addWidget(m_tree);

  connect(m_tree, &QTreeView::customContextMenuRequested, this, &FilesystemWidget::OpenMenu);

  FillTree();
}

FilesystemWidget::~FilesystemWidget() = default;

void FilesystemWidget::FillTree()
{
  // Every file and folder row uses one of these two.
  m_folder_icon = Resources::GetResourceIcon("isoproperties_folder");
  m_file_icon = Resources::GetResourceIcon("isoproperties_file");

  auto* const disc = new QStandardItem(tr("Disc"));
  disc->setEditable(false);
  disc->setIcon(Resources::GetResourceIcon("isoproperties_disc"));
  disc->setData(QVariant::fromValue(NodeKind::Disc), ROLE_KIND);
  m_model->appendRow(disc);
  m_tree->expand(disc->index());

  const std::vector<DiscIO::Partition>& partitions = m_volume->GetPartitions();
  for (size_t index = 0; index < partitions.size(); ++index)
  {
    auto* const node = new QStandardItem;
    node->setEditable(false);
    node->setIcon(Resources::GetResourceIcon("isoproperties_disc"));
    node->setData(static_cast<qlonglong>(index), ROLE_PARTITION);
    node->setData(QVariant::fromValue(NodeKind::Partition), ROLE_KIND);

    AddPartitionContents(static_cast<int>(index), node, partitions[index]);
    disc->appendRow(node);

    // The game partition starts expanded.
    if (m_volume->GetGamePartition() == partitions[index])
      m_tree->expand(node->index());
  }

  // Discs without partitions (GameCube) hang their files off the disc node.
  if (partitions.empty())
    AddPartitionContents(-1, disc, DiscIO::PARTITION_NONE);
}

void FilesystemWidget::AddPartitionContents(int partition_index, QStandardItem* parent,
                                            const DiscIO::Partition& partition)
{
  const std::optional<u32> type = m_volume->GetPartitionType(partition);
  const std::string game_id = m_volume->GetGameID(partition);
  const std::optional<u64> title_id = m_volume->GetTitleID(partition);

  // The node's label: "<existing text> - <type> - <game ID> - <title ID> (<4CC>)".
  QString label = parent->text();
  if (!label.isEmpty())
    label += QStringLiteral(" - ");

  if (type)
  {
    const u32 value = *type;
    QString type_name;
    if (value == DiscIO::PARTITION_DATA)
      type_name = tr("Data Partition (%1)").arg(value);
    else if (value == DiscIO::PARTITION_UPDATE)
      type_name = tr("Update Partition (%1)").arg(value);
    else if (value == DiscIO::PARTITION_CHANNEL)
      type_name = tr("Channel Partition (%1)").arg(value);
    else if (value == DiscIO::PARTITION_INSTALL)
      type_name = tr("Install Partition (%1)").arg(value);
    else
      type_name = tr("Other Partition (%1)").arg(value, 8, 16, QLatin1Char('0'));
    label += type_name + QStringLiteral(" - ");
  }

  label += QString::fromStdString(game_id);

  if (title_id)
  {
    const u64 id = *title_id;
    label += QStringLiteral(" - %1 (").arg(id, 16, 16, QLatin1Char('0'));
    // The low four bytes as characters, with dots for unprintable ones.
    for (int shift = 24; shift >= 0; shift -= 8)
    {
      const char letter = static_cast<char>(id >> shift);
      label += QLatin1Char(Common::IsPrintableCharacter(letter) ? letter : '.');
    }
    label += QLatin1Char(')');
  }

  parent->setText(label);

  if (const DiscIO::FileSystem* const file_system = m_volume->GetFileSystem(partition))
    AddFolderContents(partition_index, parent, file_system->GetRoot());
}

void FilesystemWidget::AddFolderContents(int partition_index, QStandardItem* parent,
                                         const DiscIO::FileInfo& folder)
{
  for (const DiscIO::FileInfo& entry : folder)
  {
    const bool is_folder = entry.IsDirectory();

    auto* const name = new QStandardItem(QString::fromStdString(entry.GetName()));
    name->setEditable(false);
    name->setIcon(is_folder ? m_folder_icon : m_file_icon);
    if (is_folder)
      AddFolderContents(partition_index, name, entry);

    name->setData(partition_index, ROLE_PARTITION);
    name->setData(QString::fromStdString(entry.GetPath()), ROLE_PATH);
    name->setData(QVariant::fromValue(is_folder ? NodeKind::Folder : NodeKind::File), ROLE_KIND);

    auto* const size =
        new QStandardItem(QString::fromStdString(UICommon::FormatSize(entry.GetTotalSize())));
    size->setTextAlignment(Qt::AlignRight);
    size->setEditable(false);

    parent->appendRow({name, size});
  }
}

QString FilesystemWidget::AskForFolder()
{
  return DolphinFileDialog::getExistingDirectory(this, QObject::tr("Choose Folder to Extract To"));
}

DiscIO::Partition FilesystemWidget::PartitionAt(int index)
{
  if (index == -1)
    return DiscIO::PARTITION_NONE;
  return m_volume->GetPartitions()[static_cast<size_t>(index)];
}

void FilesystemWidget::OpenMenu(const QPoint&)
{
  QItemSelectionModel* const selection = m_tree->selectionModel();
  if (!selection->hasSelection())
    return;

  QStandardItem* const item = m_model->itemFromIndex(selection->selectedIndexes()[0]);

  auto* const menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  const NodeKind kind = item->data(ROLE_KIND).value<NodeKind>();
  const DiscIO::Partition partition =
      kind == NodeKind::Disc ? DiscIO::PARTITION_NONE :
                               PartitionAt(item->data(ROLE_PARTITION).toInt());
  const QString path = item->data(ROLE_PATH).toString();

  // A partition, or a disc without partitions, is the top of a filesystem.
  const bool top_level =
      kind == NodeKind::Partition || (kind == NodeKind::Disc && m_volume->GetPartitions().empty());

  if (kind == NodeKind::Folder || top_level)
  {
    menu->addAction(tr("Extract Files..."), this, [this, partition, path] {
      const QString destination = AskForFolder();
      if (!destination.isEmpty())
        SaveFolder(partition, path, destination);
    });
  }

  if (top_level)
  {
    menu->addAction(tr("Extract System Data..."), this, [this, partition] {
      const QString destination = AskForFolder();
      if (destination.isEmpty())
        return;
      if (SaveSystemData(partition, destination))
      {
        ModalMessageBox::information(this, tr("Success"),
                                     tr("Successfully extracted system data."));
      }
      else
      {
        ModalMessageBox::critical(this, tr("Error"), tr("Failed to extract system data."));
      }
    });
  }

  if (kind == NodeKind::Disc)
  {
    menu->addAction(tr("Extract Entire Disc..."), this, [this] {
      const QString destination = AskForFolder();
      if (destination.isEmpty())
        return;

      if (m_volume->GetPartitions().empty())
      {
        SavePartition(DiscIO::PARTITION_NONE, destination);
        return;
      }
      // Each partition with a known type goes to a folder named after it.
      for (DiscIO::Partition& each : m_volume->GetPartitions())
      {
        const std::optional<u32> type = m_volume->GetPartitionType(each);
        if (!type)
          continue;
        const std::string folder_name = DiscIO::NameForPartitionType(*type, true);
        SavePartition(each, destination + QChar(u'/') + QString::fromStdString(folder_name));
      }
    });
  }
  else if (kind == NodeKind::Partition)
  {
    menu->addAction(tr("Extract Entire Partition..."), this, [this, partition] {
      const QString destination = AskForFolder();
      if (!destination.isEmpty())
        SavePartition(partition, destination);
    });
  }
  else if (kind == NodeKind::File)
  {
    menu->addAction(tr("Extract File..."), this, [this, partition, path] {
      const QString destination = DolphinFileDialog::getSaveFileName(this, tr("Save File To"),
                                                                     QFileInfo(path).fileName());
      if (!destination.isEmpty())
        SaveFile(partition, path, destination);
    });
  }

  menu->exec(QCursor::pos());
}

void FilesystemWidget::SavePartition(const DiscIO::Partition& partition,
                                     const QString& destination)
{
  // Files go to <destination>/files, system data to <destination>/sys.
  SaveFolder(partition, QString{}, destination + QStringLiteral("/files"));
  SaveSystemData(partition, destination);
}

bool FilesystemWidget::SaveSystemData(const DiscIO::Partition& partition,
                                      const QString& destination)
{
  return DiscIO::ExportSystemData(*m_volume, partition, destination.toStdString());
}

void FilesystemWidget::SaveFolder(const DiscIO::Partition& partition, const QString& path,
                                  const QString& destination)
{
  const DiscIO::FileSystem* const file_system = m_volume->GetFileSystem(partition);
  if (file_system == nullptr)
    return;

  const std::unique_ptr<DiscIO::FileInfo> folder = file_system->FindFileInfo(path.toStdString());
  const u32 entry_count = folder->GetTotalChildren();

  ParallelProgressDialog progress_dialog(this);
  progress_dialog.GetRaw()->setMinimum(0);
  progress_dialog.GetRaw()->setMaximum(static_cast<int>(entry_count));
  progress_dialog.GetRaw()->setWindowTitle(tr("Progress"));

  // An empty path means the whole partition.
  const bool whole = path.isEmpty();

  std::future<void> worker = std::async(std::launch::async, [&] {
    int done = 0;
    DiscIO::ExportDirectory(
        *m_volume, partition, *folder, true, path.toStdString(), destination.toStdString(),
        [whole, &progress_dialog, &done](const std::string& current) {
          const QString heading = whole ? QObject::tr("Extracting All Files...") :
                                          QObject::tr("Extracting Directory...");
          progress_dialog.SetLabelText(
              QString(heading).append(QStringLiteral(" %1").arg(QString::fromStdString(current))));
          progress_dialog.SetValue(++done);
          QCoreApplication::processEvents();
          return progress_dialog.WasCanceled();
        });
    progress_dialog.Reset();
  });

  progress_dialog.GetRaw()->exec();
  worker.get();
}

void FilesystemWidget::SaveFile(const DiscIO::Partition& partition, const QString& path,
                                const QString& destination)
{
  const DiscIO::FileSystem* const file_system = m_volume->GetFileSystem(partition);
  if (file_system == nullptr)
    return;

  const bool ok =
      DiscIO::ExportFile(*m_volume, partition, file_system->FindFileInfo(path.toStdString()).get(),
                         destination.toStdString());
  if (ok)
    ModalMessageBox::information(this, tr("Success"), tr("Successfully extracted file."));
  else
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to extract file."));
}
