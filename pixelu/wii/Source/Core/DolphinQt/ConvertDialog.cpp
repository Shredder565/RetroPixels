// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/ConvertDialog.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <system_error>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/ScrubbedBlob.h"
#include "DiscIO/WIABlob.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"

namespace
{
using DiscIO::BlobType;
using DiscIO::WIARVZCompressionType;

// The compression level picked by default when it is on offer.
constexpr int DEFAULT_COMPRESSION_LEVEL = 5;

QString FileOf(const std::string& path)
{
  return QString::fromStdString(path);
}

// "<folder of source>/<base name of source><extension>"
QString OutputPathNextTo(const std::string& source, const QString& extension)
{
  const QFileInfo info(FileOf(source));
  return info.dir().absoluteFilePath(info.completeBaseName()).append(extension);
}
}  // namespace

ConvertDialog::ConvertDialog(QList<std::shared_ptr<const UICommon::GameFile>> files,
                             QWidget* parent)
    : QDialog(parent), m_files(std::move(files))
{
  ASSERT(!m_files.empty());

  setWindowTitle(tr("Convert"));

  auto* const form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  m_format = new QComboBox;
  const std::pair<const char*, BlobType> formats[] = {
      {"ISO", BlobType::PLAIN},
      {"GCZ", BlobType::GCZ},
      {"WIA", BlobType::WIA},
      {"RVZ", BlobType::RVZ},
  };
  for (const auto& [name, type] : formats)
    m_format->addItem(QString::fromLatin1(name), static_cast<int>(type));
  // Plain images are usually being compressed, so RVZ is the likely choice for them.
  const bool all_plain = std::ranges::all_of(
      m_files, [](const auto& file) { return file->GetBlobType() == BlobType::PLAIN; });
  if (all_plain)
    m_format->setCurrentIndex(m_format->count() - 1);
  form->addRow(tr("Format:"), m_format);

  m_block_size = new QComboBox;
  form->addRow(tr("Block Size:"), m_block_size);

  m_compression = new QComboBox;
  form->addRow(tr("Compression:"), m_compression);

  m_compression_level = new QComboBox;
  form->addRow(tr("Compression Level:"), m_compression_level);

  m_scrub = new QCheckBox;
  form->addRow(tr("Remove Junk Data (Irreversible):"), m_scrub);

  auto* const convert_button = new QPushButton(tr("Convert..."));

  auto* const explanation = new QLabel(
      tr("ISO: A simple and robust format which is supported by many programs. It takes "
         "up more space than any other format.\n\n"
         "GCZ: A basic compressed format which is compatible with most versions of "
         "Dolphin and some other programs. It can't efficiently compress junk data "
         "(unless removed) or encrypted Wii data.\n\n"
         "WIA: An advanced compressed format which is compatible with Dolphin 5.0-12188 "
         "and later, and a few other programs. It can efficiently compress encrypted "
         "Wii data, but not junk data (unless removed).\n\n"
         "RVZ: An advanced compressed format which is compatible with Dolphin 5.0-12188 "
         "and later. It can efficiently compress both junk data and encrypted Wii data."));
  explanation->setWordWrap(true);
  explanation->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

  auto* const options = new QGroupBox(tr("Options"));
  auto* const options_layout = new QVBoxLayout{options};
  options_layout->addLayout(form);
  options_layout->addWidget(convert_button);

  auto* const info = new QGroupBox(tr("Info"));
  auto* const info_layout = new QVBoxLayout{info};
  info_layout->addWidget(explanation);

  auto* const layout = new QVBoxLayout{this};
  layout->addWidget(options);
  layout->addWidget(info, 1);

  connect(m_format, &QComboBox::currentIndexChanged, this, &ConvertDialog::OnFormatChanged);
  connect(m_compression, &QComboBox::currentIndexChanged, this,
          &ConvertDialog::OnCompressionChanged);
  connect(convert_button, &QPushButton::clicked, this, &ConvertDialog::Convert);

  OnFormatChanged();
  OnCompressionChanged();
}

void ConvertDialog::AddBlockSize(int size)
{
  m_block_size->addItem(QString::fromStdString(UICommon::FormatSize(static_cast<u64>(size), 0)), size);

  // Sizes arrive smallest first, so this ends up on the preferred size, or the largest one
  // below it when the preferred size is not on offer.
  if (size <= DiscIO::GCZ_RVZ_PREFERRED_BLOCK_SIZE)
    m_block_size->setCurrentIndex(m_block_size->count() - 1);
}

void ConvertDialog::FillBlockSizes(BlobType format)
{
  switch (format)
  {
  case BlobType::GCZ:
  {
    // Old Dolphin versions can't read every GCZ block size (see
    // DiscIO::IsGCZBlockSizeLegacyCompatible), so only sizes that suit every file are offered.
    const auto suits_all = [this](int size) {
      return std::ranges::all_of(m_files, [size](const auto& file) {
        return DiscIO::IsGCZBlockSizeLegacyCompatible(size, file->GetVolumeSize());
      });
    };
    for (int size = DiscIO::PREFERRED_MIN_BLOCK_SIZE; size <= DiscIO::PREFERRED_MAX_BLOCK_SIZE;
         size *= 2)
    {
      if (suits_all(size))
        AddBlockSize(size);
    }

    // Nothing suits: fall back to the size old versions always used, which is no worse than
    // what those versions wrote themselves.
    if (m_block_size->count() == 0)
    {
      if (!suits_all(DiscIO::GCZ_FALLBACK_BLOCK_SIZE))
      {
        ERROR_LOG_FMT(MASTER_LOG, "Failed to find a block size which does not cause problems "
                                  "when decompressing using an old version of Dolphin");
      }
      AddBlockSize(DiscIO::GCZ_FALLBACK_BLOCK_SIZE);
    }
    return;
  }
  case BlobType::WIA:
    // The smallest size WIA allows; larger ones are slower.
    AddBlockSize(DiscIO::WIA_MIN_BLOCK_SIZE);
    return;
  case BlobType::RVZ:
    for (int size = DiscIO::PREFERRED_MIN_BLOCK_SIZE; size <= DiscIO::PREFERRED_MAX_BLOCK_SIZE;
         size *= 2)
    {
      AddBlockSize(size);
    }
    return;
  default:
    return;
  }
}

void ConvertDialog::FillCompressionMethods(BlobType format)
{
  const auto add = [this](const QString& name, WIARVZCompressionType type) {
    m_compression->addItem(name, static_cast<int>(type));
  };

  if (format == BlobType::GCZ)
  {
    add(QStringLiteral("Deflate"), WIARVZCompressionType::None);
    return;
  }
  if (format != BlobType::WIA && format != BlobType::RVZ)
    return;

  // i18n: %1 is the name of a compression method (e.g. LZMA)
  const QString slow = tr("%1 (slow)");

  add(tr("No Compression"), WIARVZCompressionType::None);
  if (format == BlobType::WIA)
    add(QStringLiteral("Purge"), WIARVZCompressionType::Purge);
  add(slow.arg(QStringLiteral("bzip2")), WIARVZCompressionType::Bzip2);
  add(slow.arg(QStringLiteral("LZMA")), WIARVZCompressionType::LZMA);
  add(slow.arg(QStringLiteral("LZMA2")), WIARVZCompressionType::LZMA2);

  if (format == BlobType::RVZ)
  {
    // i18n: %1 is the name of a compression method (e.g. Zstandard)
    const QString recommended = tr("%1 (recommended)");
    add(recommended.arg(QStringLiteral("Zstandard")), WIARVZCompressionType::Zstd);
    m_compression->setCurrentIndex(m_compression->count() - 1);
  }
}

void ConvertDialog::OnFormatChanged()
{
  const auto format = static_cast<BlobType>(m_format->currentData().toInt());

  m_block_size->clear();
  m_compression->clear();
  FillBlockSizes(format);
  FillCompressionMethods(format);

  // A choice of one is no choice.
  m_block_size->setEnabled(m_block_size->count() > 1);
  m_compression->setEnabled(m_compression->count() > 1);

  // RVZ stores junk data efficiently by itself, and Datel discs can't be scrubbed.
  const bool can_scrub =
      format != BlobType::RVZ && std::ranges::none_of(m_files, &UICommon::GameFile::IsDatelDisc);
  m_scrub->setEnabled(can_scrub);
  if (!can_scrub)
    m_scrub->setChecked(false);
}

void ConvertDialog::OnCompressionChanged()
{
  m_compression_level->clear();

  const auto method = static_cast<WIARVZCompressionType>(m_compression->currentData().toInt());
  const auto [lowest, highest] = DiscIO::GetAllowedCompressionLevels(method, true);
  for (int level = lowest; level <= highest; ++level)
  {
    m_compression_level->addItem(QString::number(level), level);
    if (level == DEFAULT_COMPRESSION_LEVEL)
      m_compression_level->setCurrentIndex(m_compression_level->count() - 1);
  }

  m_compression_level->setEnabled(m_compression_level->count() > 1);
}

bool ConvertDialog::Confirm(const QString& question)
{
  ModalMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(tr("Confirm"));
  box.setText(tr("Are you sure?"));
  box.setInformativeText(question);
  box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
  return box.exec() == QMessageBox::Yes;
}

bool ConvertDialog::ConfirmSettings(BlobType format, bool scrub)
{
  if (scrub && format == BlobType::PLAIN &&
      !Confirm(tr("Removing junk data does not save any space when converting to "
                  "ISO (unless you package the ISO file in a compressed file format "
                  "such as ZIP afterwards). Do you want to continue anyway?")))
  {
    return false;
  }

  const auto is_wii_disc = [](const auto& file) {
    return file->GetPlatform() == DiscIO::Platform::WiiDisc && !file->IsDatelDisc();
  };
  if (!scrub && format == BlobType::GCZ && std::ranges::any_of(m_files, is_wii_disc) &&
      !Confirm(tr("Converting Wii disc images to GCZ without removing junk data "
                  "does not save any noticeable amount of space compared to "
                  "converting to ISO. Do you want to continue anyway?")))
  {
    return false;
  }

  if (std::ranges::any_of(m_files, &UICommon::GameFile::IsNKit) &&
      !Confirm(tr("Dolphin can't convert NKit files to non-NKit files. Converting an NKit file in "
                  "Dolphin will result in another NKit file.\n"
                  "\n"
                  "If you want to convert an NKit file to a non-NKit file, you can use the same "
                  "program as you originally used when converting the file to the NKit format.\n"
                  "\n"
                  "Do you want to continue anyway?")))
  {
    return false;
  }

  return true;
}

void ConvertDialog::Convert()
{
  const auto format = static_cast<BlobType>(m_format->currentData().toInt());
  const int block_size = m_block_size->currentData().toInt();
  const auto compression =
      static_cast<WIARVZCompressionType>(m_compression->currentData().toInt());
  const int compression_level = m_compression_level->currentData().toInt();
  const bool scrub = m_scrub->isChecked();

  if (!ConfirmSettings(format, scrub))
    return;

  QString extension;
  QString filter;
  switch (format)
  {
  case BlobType::PLAIN:
    extension = QStringLiteral(".iso");
    filter = tr("Uncompressed GC/Wii images (*.iso *.gcm)");
    break;
  case BlobType::GCZ:
    extension = QStringLiteral(".gcz");
    filter = tr("GCZ GC/Wii images (*.gcz)");
    break;
  case BlobType::WIA:
    extension = QStringLiteral(".wia");
    filter = tr("WIA GC/Wii images (*.wia)");
    break;
  case BlobType::RVZ:
    extension = QStringLiteral(".rvz");
    filter = tr("RVZ GC/Wii images (*.rvz)");
    break;
  default:
    ASSERT(false);
    return;
  }

  // Several files go to a folder under their own names; a single file gets a full save path.
  const bool batch = m_files.size() > 1;
  const std::string& first_source = m_files[0]->GetFilePath();
  QString target_dir;
  QString target;
  if (batch)
  {
    target_dir = DolphinFileDialog::getExistingDirectory(
        this, tr("Save Converted Images"), QFileInfo(FileOf(first_source)).dir().absolutePath());
    if (target_dir.isEmpty())
      return;
  }
  else
  {
    target = DolphinFileDialog::getSaveFileName(this, tr("Save Converted Image"),
                                                OutputPathNextTo(first_source, extension), filter);
    if (target.isEmpty())
      return;
  }

  int converted = 0;

  for (const auto& file : m_files)
  {
    const std::string source = file->GetFilePath();

    if (batch)
    {
      target = QDir(target_dir)
                   .absoluteFilePath(QFileInfo(FileOf(source)).completeBaseName())
                   .append(extension);

      const QFileInfo existing(target);
      if (existing.exists())
      {
        ModalMessageBox replace(this);
        replace.setIcon(QMessageBox::Warning);
        replace.setWindowTitle(tr("Confirm"));
        replace.setText(tr("The file %1 already exists.\n"
                           "Do you wish to replace it?")
                            .arg(existing.fileName()));
        replace.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        if (replace.exec() == QMessageBox::No)
          continue;
      }
    }

    const std::filesystem::path target_fs = StringToPath(target.toStdString());
    if (std::filesystem::exists(target_fs))
    {
      std::error_code error;
      if (std::filesystem::equivalent(target_fs, StringToPath(source), error))
      {
        ModalMessageBox::critical(
            this, tr("Error"),
            tr("The destination file cannot be the same as the source file\n\n"
               "Please select another destination path for \"%1\"")
                .arg(FileOf(source)));
        continue;
      }
    }

    ParallelProgressDialog progress(tr("Converting..."), tr("Abort"), 0, 100, this);
    progress.GetRaw()->setWindowModality(Qt::WindowModal);
    progress.GetRaw()->setWindowTitle(tr("Progress"));
    if (batch)
    {
      // i18n: %1 is a filename.
      progress.GetRaw()->setLabelText(
          tr("Converting...\n%1").arg(QFileInfo(FileOf(source)).fileName()));
    }

    // Removing junk data needs a special reader; if that fails, the user may go on without it.
    std::unique_ptr<DiscIO::BlobReader> reader;
    bool scrub_this = scrub;
    if (scrub_this)
    {
      reader = DiscIO::ScrubbedBlob::Create(source);
      if (!reader)
      {
        const int answer = ModalMessageBox::warning(
            this, tr("Question"),
            tr("Failed to remove junk data from file \"%1\".\n\n"
               "Would you like to convert it without removing junk data?")
                .arg(FileOf(source)),
            QMessageBox::Ok | QMessageBox::Abort);
        if (answer != QMessageBox::Ok)
          return;
        scrub_this = false;
      }
    }
    if (!scrub_this)
      reader = DiscIO::CreateBlobReader(source);

    if (!reader)
    {
      ModalMessageBox::critical(
          this, tr("Error"),
          tr("Failed to open the input file \"%1\".").arg(FileOf(source)));
      return;
    }

    const auto report = [&progress](const std::string&, float fraction) {
      progress.SetValue(static_cast<int>(fraction * 100));
      return !progress.WasCanceled();
    };

    const std::string destination_path = target.toStdString();
    std::function<bool()> job;
    switch (format)
    {
    case BlobType::PLAIN:
      job = [&] { return DiscIO::ConvertToPlain(reader.get(), source, destination_path, report); };
      break;
    case BlobType::GCZ:
      job = [&] {
        const u32 sub_type = file->GetPlatform() == DiscIO::Platform::WiiDisc ? 1 : 0;
        return DiscIO::ConvertToGCZ(reader.get(), source, destination_path, sub_type, block_size,
                                    report);
      };
      break;
    case BlobType::WIA:
    case BlobType::RVZ:
      job = [&] {
        return DiscIO::ConvertToWIAOrRVZ(reader.get(), source, destination_path,
                                         format == BlobType::RVZ, compression, compression_level,
                                         block_size, report);
      };
      break;
    default:
      ASSERT(false);
      break;
    }

    // The conversion runs on its own thread while the progress dialog runs the event loop here;
    // closing the dialog when the job ends lets exec() return.
    std::future<bool> outcome = std::async(std::launch::async, [&] {
      const bool ok = job();
      progress.Reset();
      return ok;
    });

    progress.GetRaw()->exec();
    if (!outcome.get())
    {
      ModalMessageBox::critical(this, tr("Error"),
                                tr("Dolphin failed to complete the requested action."));
      return;
    }

    ++converted;
  }

  ModalMessageBox::information(this, tr("Success"),
                               tr("Successfully converted %n image(s).", "", converted));
  close();
}
