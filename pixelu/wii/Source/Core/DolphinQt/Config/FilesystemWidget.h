// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include <QDialog>
#include <QIcon>

class QStandardItem;
class QStandardItemModel;
class QTreeView;

namespace DiscIO
{
class FileInfo;
class Volume;

struct Partition;
}  // namespace DiscIO

// The Filesystem tab: a tree of the disc's partitions, folders and files,
// with a context menu for extracting them.
class FilesystemWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit FilesystemWidget(std::shared_ptr<DiscIO::Volume> volume);
  ~FilesystemWidget() override;

private:
  void FillTree();
  void AddPartitionContents(int partition_index, QStandardItem* parent,
                            const DiscIO::Partition& partition);
  void AddFolderContents(int partition_index, QStandardItem* parent,
                         const DiscIO::FileInfo& folder);

  void OpenMenu(const QPoint& position);
  QString AskForFolder();
  DiscIO::Partition PartitionAt(int index);

  void SavePartition(const DiscIO::Partition& partition, const QString& destination);
  void SaveFolder(const DiscIO::Partition& partition, const QString& path,
                  const QString& destination);
  void SaveFile(const DiscIO::Partition& partition, const QString& path,
                const QString& destination);
  bool SaveSystemData(const DiscIO::Partition& partition, const QString& destination);

  QStandardItemModel* m_model;
  QTreeView* m_tree;

  std::shared_ptr<DiscIO::Volume> m_volume;

  QIcon m_folder_icon;
  QIcon m_file_icon;
};
