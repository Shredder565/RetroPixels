// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include <QDialog>
#include <QList>

#include "DiscIO/Blob.h"

class QCheckBox;
class QComboBox;

namespace DiscIO
{
enum class WIARVZCompressionType : u32;
}

namespace UICommon
{
class GameFile;
}

// Converts one or more disc images to ISO, GCZ, WIA or RVZ.
class ConvertDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit ConvertDialog(QList<std::shared_ptr<const UICommon::GameFile>> files,
                         QWidget* parent = nullptr);

private:
  void FillBlockSizes(DiscIO::BlobType format);
  void FillCompressionMethods(DiscIO::BlobType format);
  void AddBlockSize(int size);
  void OnFormatChanged();
  void OnCompressionChanged();
  bool Confirm(const QString& question);
  bool ConfirmSettings(DiscIO::BlobType format, bool scrub);
  void Convert();

  QComboBox* m_format = nullptr;
  QComboBox* m_block_size = nullptr;
  QComboBox* m_compression = nullptr;
  QComboBox* m_compression_level = nullptr;
  QCheckBox* m_scrub = nullptr;
  QList<std::shared_ptr<const UICommon::GameFile>> m_files;
};
