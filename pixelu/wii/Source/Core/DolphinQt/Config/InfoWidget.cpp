// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/InfoWidget.h"

#include <fmt/format.h>

#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "Core/ConfigManager.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/WiiSaveBanner.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ImageConverter.h"
#include "UICommon/UICommon.h"

namespace
{
QFormLayout* NewForm()
{
  auto* const form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  return form;
}
}  // namespace

InfoWidget::InfoWidget(const UICommon::GameFile& game) : m_game(game)
{
  m_volume = DiscIO::CreateVolume(m_game.GetFilePath());

  auto* const column = new QVBoxLayout();
  column->addWidget(FileSection());
  column->addWidget(GameSection());
  // Only titles with banner text get the banner section.
  if (!game.GetLanguages().empty())
    column->addWidget(BannerSection());
  column->addStretch(1);
  setLayout(column);
}

InfoWidget::~InfoWidget() = default;

QLineEdit* InfoWidget::ReadOnlyField(const QString& text)
{
  auto* const field = new QLineEdit(text, this);
  field->setReadOnly(true);
  field->setCursorPosition(0);
  return field;
}

QLineEdit* InfoWidget::ReadOnlyField(const std::string& text)
{
  return ReadOnlyField(QString::fromStdString(text));
}

QGroupBox* InfoWidget::FileSection()
{
  auto* const group = new QGroupBox(tr("File Details"));
  QFormLayout* const form = NewForm();

  form->addRow(tr("Path:"), ReadOnlyField(QDir::toNativeSeparators(
                                QString::fromStdString(m_game.GetFilePath()))));

  const std::string size = UICommon::FormatSize(m_game.GetFileSize());
  if (m_game.ShouldShowFileFormatDetails())
  {
    // Containers (RVZ, WIA, ...) show their format, compression and block
    // size; plain images only their size.
    const QString format = QStringLiteral("%1 (%2)")
                               .arg(QString::fromStdString(m_game.GetFileFormatName()))
                               .arg(QString::fromStdString(size));
    form->addRow(tr("File Format:"), ReadOnlyField(format));

    QString compression = QString::fromStdString(m_game.GetCompressionMethod());
    if (compression.isEmpty())
      compression = tr("No Compression");
    form->addRow(tr("Compression:"), ReadOnlyField(compression));

    if (m_game.GetBlockSize() > 0)
    {
      form->addRow(tr("Block Size:"),
                   ReadOnlyField(UICommon::FormatSize(m_game.GetBlockSize(), 0)));
    }
  }
  else
  {
    form->addRow(tr("File Size:"), ReadOnlyField(size));
  }

  group->setLayout(form);
  return group;
}

QGroupBox* InfoWidget::GameSection()
{
  const QString unknown = tr("Unknown");

  auto* const group = new QGroupBox(tr("Game Details"));
  QFormLayout* const form = NewForm();

  const QString internal_name = QString::fromStdString(m_game.GetInternalName());
  const QString shown_name = internal_name.isEmpty() ? unknown : internal_name;

  const auto platform_is = [this](DiscIO::Platform platform) {
    return m_game.GetPlatform() == platform;
  };
  const bool from_disc = platform_is(DiscIO::Platform::GameCubeDisc) ||
                         platform_is(DiscIO::Platform::Triforce) ||
                         platform_is(DiscIO::Platform::WiiDisc);

  // Only disc images have a disc number.
  QString name_line;
  if (from_disc)
  {
    name_line = tr("%1 (Disc %2, Revision %3)")
                    .arg(shown_name)
                    .arg(m_game.GetDiscNumber() + 1)
                    .arg(m_game.GetRevision());
  }
  else
  {
    name_line = tr("%1 (Revision %3)").arg(shown_name).arg(m_game.GetRevision());
  }
  QLineEdit* const name_field = ReadOnlyField(name_line);

  QString id_line = QString::fromStdString(m_game.GetGameID());
  const u64 title_id = m_game.GetTitleID();
  if (title_id != 0)
    id_line += QStringLiteral(" (%1)").arg(title_id, 16, 16, QLatin1Char('0'));
  QLineEdit* const id_field = ReadOnlyField(id_line);

  QLineEdit* const country_field = ReadOnlyField(DiscIO::GetName(m_game.GetCountry(), true));

  std::string maker = m_game.GetMaker(UICommon::GameFile::Variant::LongAndNotCustom);
  if (maker.empty())
    maker = unknown.toStdString();
  QLineEdit* const maker_field = ReadOnlyField(maker + " (" + m_game.GetMakerID() + ")");

  form->addRow(tr("Name:"), name_field);
  form->addRow(tr("Game ID:"), id_field);
  form->addRow(tr("Country:"), country_field);
  form->addRow(tr("Maker:"), maker_field);

  if (!m_game.GetApploaderDate().empty())
    form->addRow(tr("Apploader Date:"), ReadOnlyField(m_game.GetApploaderDate()));

  // Wii titles name the IOS they run on in their TMD.
  if (m_volume)
  {
    const IOS::ES::TMDReader& tmd = m_volume->GetTMD(m_volume->GetGamePartition());
    if (tmd.IsValid())
    {
      form->addRow(tr("IOS Version:"),
                   ReadOnlyField(fmt::format("IOS{}", static_cast<u32>(tmd.GetIOSId()))));
    }
  }

  group->setLayout(form);
  return group;
}

QGroupBox* InfoWidget::BannerSection()
{
  auto* const group = new QGroupBox(tr("Banner Details"));
  QFormLayout* const form = NewForm();

  BuildLanguageBox();
  form->addRow(tr("Show Language:"), m_language);

  // GameCube banners carry name, maker and description; Wii banners only a
  // name.
  if (m_game.GetPlatform() == DiscIO::Platform::GameCubeDisc)
  {
    m_banner_name = ReadOnlyField();
    form->addRow(tr("Name:"), m_banner_name);
    m_banner_maker = ReadOnlyField();
    form->addRow(tr("Maker:"), m_banner_maker);
    m_banner_description = new QTextEdit();
    form->addRow(tr("Description:"), m_banner_description);
    m_banner_description->setReadOnly(true);
  }
  else if (DiscIO::IsWii(m_game.GetPlatform()))
  {
    m_banner_name = ReadOnlyField();
    form->addRow(tr("Name:"), m_banner_name);
  }

  ShowBannerText();

  const QPixmap image = ToQPixmap(m_game.GetBannerImage());
  if (!image.isNull())
    form->addRow(tr("Banner:"), BannerPreview(image));

  group->setLayout(form);
  return group;
}

QWidget* InfoWidget::BannerPreview(const QPixmap& image)
{
  auto* const row = new QWidget();
  auto* const row_layout = new QHBoxLayout();

  // Never shown larger than a Wii save banner.
  const QSize limit{DiscIO::WiiSaveBanner::BANNER_WIDTH, DiscIO::WiiSaveBanner::BANNER_HEIGHT};
  auto* const picture = new QLabel();
  picture->setPixmap(image.scaled(image.size().boundedTo(limit)));

  auto* const export_button = new QPushButton(tr("Save as..."));
  connect(export_button, &QPushButton::clicked, this, &InfoWidget::ExportBanner);

  row_layout->addWidget(picture);
  row_layout->addWidget(export_button);
  row->setLayout(row_layout);
  return row;
}

void InfoWidget::ExportBanner()
{
  const QString target =
      DolphinFileDialog::getSaveFileName(this, tr("Select a File"), QDir::currentPath(),
                                         tr("PNG image file (*.png);; All Files (*)"));
  ToQPixmap(m_game.GetBannerImage()).save(target, "PNG");
}

void InfoWidget::BuildLanguageBox()
{
  const bool wii = DiscIO::IsWii(m_game.GetPlatform());
  const DiscIO::Language preferred = SConfig::GetInstance().GetCurrentLanguage(wii);

  // Starts on the configured language when the banner has it.
  m_language = new QComboBox();
  for (const DiscIO::Language language : m_game.GetLanguages())
  {
    m_language->addItem(QString::fromStdString(DiscIO::GetName(language, true)),
                        static_cast<int>(language));
    if (language == preferred)
      m_language->setCurrentIndex(m_language->count() - 1);
  }
  if (m_language->count() == 1)
    m_language->setDisabled(true);

  connect(m_language, &QComboBox::currentIndexChanged, this, &InfoWidget::ShowBannerText);
}

void InfoWidget::ShowBannerText()
{
  const auto language = static_cast<DiscIO::Language>(m_language->currentData().toInt());

  if (m_banner_name != nullptr)
    m_banner_name->setText(QString::fromStdString(m_game.GetLongName(language)));
  if (m_banner_maker != nullptr)
    m_banner_maker->setText(QString::fromStdString(m_game.GetLongMaker(language)));
  if (m_banner_description != nullptr)
    m_banner_description->setText(QString::fromStdString(m_game.GetDescription(language)));
}
