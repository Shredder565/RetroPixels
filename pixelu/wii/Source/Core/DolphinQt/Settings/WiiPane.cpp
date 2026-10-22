// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/WiiPane.h"

#include <array>
#include <future>
#include <optional>
#include <utility>
#include <vector>

#include <QDir>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/USBUtils.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigUserPath.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Settings/USBDevicePicker.h"

namespace
{
constexpr u64 ONE_MIB = u64{1} << 20;
constexpr u64 ONE_GIB = u64{1} << 30;

struct CardSize
{
  const char* label;  // untranslated; translated in the WiiPane context when shown
  u64 bytes;
};

// Sizes offered for a newly created SD card image. Zero lets the core choose.
constexpr std::array CARD_SIZES{
    CardSize{_trans("Auto"), 0},
    CardSize{_trans("64 MiB"), 64 * ONE_MIB},
    CardSize{_trans("128 MiB"), 128 * ONE_MIB},
    CardSize{_trans("256 MiB"), 256 * ONE_MIB},
    CardSize{_trans("512 MiB"), 512 * ONE_MIB},
    CardSize{_trans("1 GiB"), ONE_GIB},
    CardSize{_trans("2 GiB"), 2 * ONE_GIB},
    CardSize{_trans("4 GiB (SDHC)"), 4 * ONE_GIB},
    CardSize{_trans("8 GiB (SDHC)"), 8 * ONE_GIB},
    CardSize{_trans("16 GiB (SDHC)"), 16 * ONE_GIB},
    CardSize{_trans("32 GiB (SDHC)"), 32 * ONE_GIB},
};

// A row holding a caption, a path field and a "..." button that runs `on_browse`.
template <typename Slot>
QHBoxLayout* PathRow(WiiPane* pane, const QString& caption, ConfigUserPath* field, Slot on_browse)
{
  auto* browse = new NonDefaultQPushButton(QStringLiteral("..."));
  QObject::connect(browse, &QPushButton::clicked, pane, on_browse);

  auto* row = new QHBoxLayout;
  row->addWidget(new QLabel(caption));
  row->addWidget(field);
  row->addWidget(browse);
  return row;
}
}  // namespace

WiiPane::WiiPane(QWidget* parent) : QWidget(parent)
{
  auto* page = new QVBoxLayout{this};
  page->addWidget(BuildSystemGroup());
  page->addWidget(BuildSDCardGroup());
  page->addWidget(BuildPassthroughGroup());
  page->addWidget(BuildRemoteGroup());

  RefreshPassthroughList();
  WireUp();
  RefreshRemoveButton();
  LockForEmulation(!Core::IsUninitialized(Core::System::GetInstance()));
}

QGroupBox* WiiPane::BuildSystemGroup()
{
  auto* box = new QGroupBox(tr("Misc Settings"));
  auto* grid = new QGridLayout();
  box->setLayout(grid);

  m_pal60 = new ConfigBool(tr("Use PAL60 Mode (EuRGB60)"), Config::SYSCONF_PAL60);
  m_screen_saver = new ConfigBool(tr("Enable Screen Saver"), Config::SYSCONF_SCREENSAVER);
  m_wiilink =
      new ConfigBool(tr("Enable WiiConnect24 via WiiLink"), Config::MAIN_WII_WIILINK_ENABLE);
  m_usb_keyboard = new ConfigBool(tr("Connect USB Keyboard"), Config::MAIN_WII_KEYBOARD);

  auto* widescreen_label = new QLabel(tr("Aspect Ratio:"));
  m_widescreen = new ConfigChoiceMap<bool>({{tr("4:3"), false}, {tr("16:9"), true}},
                                           Config::SYSCONF_WIDESCREEN);

  auto* language_label = new QLabel(tr("System Language:"));
  // Order matches the SYSCONF language numbering.
  const QStringList languages{tr("Japanese"),
                              tr("English"),
                              tr("German"),
                              tr("French"),
                              tr("Spanish"),
                              tr("Italian"),
                              tr("Dutch"),
                              tr("Simplified Chinese"),
                              tr("Traditional Chinese"),
                              tr("Korean")};
  m_language = new ConfigChoiceU32(languages, Config::SYSCONF_LANGUAGE);

  auto* sound_label = new QLabel(tr("Sound:"));
  // i18n: Mono audio
  QString mono = tr("Mono");
  // i18n: Stereo audio
  QString stereo = tr("Stereo");
  // i18n: Surround audio (Dolby Pro Logic II)
  QString surround = tr("Surround");
  m_sound = new ConfigChoiceU32({mono, stereo, surround}, Config::SYSCONF_SOUND_MODE);

  m_pal60->SetDescription(tr("Sets the Wii display mode to 60Hz (480i) instead of 50Hz (576i) "
                             "for PAL games.\nMay not work for all games."));
  m_screen_saver->SetDescription(tr("Dims the screen after five minutes of inactivity."));
  m_wiilink->SetDescription(
      tr("Enables the WiiLink service for WiiConnect24 channels.\nWiiLink is an alternate "
         "provider for the discontinued WiiConnect24 Channels such as the Forecast and Nintendo "
         "Channels\nRead the Terms of Service at: https://www.wiilink24.com/tos"));
  m_language->SetDescription(tr("Sets the Wii system language."));
  m_usb_keyboard->SetDescription(tr("May cause slow down in Wii Menu and some games."));

  // Two columns: toggles on top, then caption/choice pairs.
  const std::array<QWidget*, 10> cells{m_pal60,          m_usb_keyboard, m_screen_saver, m_wiilink,
                                       widescreen_label, m_widescreen,   language_label, m_language,
                                       sound_label,      m_sound};
  for (int i = 0; i < static_cast<int>(cells.size()); ++i)
    grid->addWidget(cells[static_cast<size_t>(i)], i / 2, i % 2, 1, 1);

  return box;
}

QGroupBox* WiiPane::BuildSDCardGroup()
{
  auto* box = new QGroupBox(tr("SD Card Settings"));
  auto* grid = new QGridLayout();
  box->setLayout(grid);

  auto* insert_card = new ConfigBool(tr("Insert SD Card"), Config::MAIN_WII_SD_CARD);
  insert_card->SetDescription(tr("Supports SD and SDHC. Default size is 128 MB."));
  auto* allow_writes = new ConfigBool(tr("Allow Writes to SD Card"), Config::MAIN_ALLOW_SD_WRITES);
  grid->addWidget(insert_card, 0, 0, 1, 1);
  grid->addWidget(allow_writes, 0, 1, 1, 1);

  m_sd_image_path = new ConfigUserPath(F_WIISDCARDIMAGE_IDX, Config::MAIN_WII_SD_CARD_IMAGE_PATH);
  grid->addLayout(PathRow(this, tr("SD Card Path:"), m_sd_image_path, &WiiPane::PickSDImage), 1, 0,
                  1, 2);

  auto* folder_sync = new ConfigBool(tr("Automatically Sync with Folder"),
                                     Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC);
  folder_sync->SetDescription(
      tr("Synchronizes the SD Card with the SD Sync Folder when starting and ending emulation."));
  grid->addWidget(folder_sync, 2, 0, 1, 2);

  m_sd_folder_path =
      new ConfigUserPath(D_WIISDCARDSYNCFOLDER_IDX, Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH);
  grid->addLayout(
      PathRow(this, tr("SD Sync Folder:"), m_sd_folder_path, &WiiPane::PickSDSyncFolder), 3, 0, 1,
      2);

  std::vector<std::pair<QString, u64>> sizes;
  sizes.reserve(CARD_SIZES.size());
  for (const CardSize& size : CARD_SIZES)
    sizes.emplace_back(tr(size.label), size.bytes);
  auto* size_choice = new ConfigChoiceMap(sizes, Config::MAIN_WII_SD_CARD_FILESIZE);
  grid->addWidget(new QLabel(tr("SD Card File Size:")), 4, 0);
  grid->addWidget(size_choice, 4, 1);

  m_sd_pack = new NonDefaultQPushButton(tr(Common::SD_PACK_TEXT));
  m_sd_unpack = new NonDefaultQPushButton(tr(Common::SD_UNPACK_TEXT));
  connect(m_sd_pack, &QPushButton::clicked, this, [this] { ConvertSDCard(true); });
  connect(m_sd_unpack, &QPushButton::clicked, this, [this] { ConvertSDCard(false); });
  grid->addWidget(m_sd_pack, 5, 0, 1, 1);
  grid->addWidget(m_sd_unpack, 5, 1, 1, 1);

  return box;
}

QGroupBox* WiiPane::BuildPassthroughGroup()
{
  m_passthrough_list = new QtUtils::MinimumSizeHintWidget<QListWidget>;
  m_passthrough_add = new NonDefaultQPushButton(tr("Add..."));
  m_passthrough_remove = new NonDefaultQPushButton(tr("Remove"));

  auto* buttons = new QHBoxLayout;
  buttons->addStretch();
  buttons->addWidget(m_passthrough_add);
  buttons->addWidget(m_passthrough_remove);

  auto* column = new QVBoxLayout;
  column->addWidget(m_passthrough_list);
  column->addLayout(buttons);

  auto* box = new QGroupBox(tr("Whitelisted USB Passthrough Devices"));
  box->setLayout(column);
  return box;
}

QGroupBox* WiiPane::BuildRemoteGroup()
{
  auto* box = new QGroupBox(tr("Wii Remote Settings"));
  auto* grid = new QGridLayout();
  box->setLayout(grid);

  m_rumble = new ConfigBool(tr("Enable Rumble"), Config::SYSCONF_WIIMOTE_MOTOR);

  auto* position_label = new QLabel(tr("Sensor Bar Position:"));
  m_sensor_bar_position = new ConfigChoiceMap<u32>({{tr("Top"), 1}, {tr("Bottom"), 0}},
                                                   Config::SYSCONF_SENSOR_BAR_POSITION);

  // i18n: IR stands for infrared and refers to the pointer functionality of Wii Remotes
  auto* sensitivity_label = new QLabel(tr("IR Sensitivity:"));
  // The system menu stores sensitivity as 1..5.
  m_ir_sensitivity = new ConfigSliderU32(1, 5, Config::SYSCONF_SENSOR_BAR_SENSITIVITY, 1);

  auto* volume_label = new QLabel(tr("Speaker Volume:"));
  m_speaker_volume = new ConfigSliderU32(0, 127, Config::SYSCONF_SPEAKER_VOLUME, 1);

  grid->addWidget(position_label, 0, 0);
  grid->addWidget(m_sensor_bar_position, 0, 1);
  grid->addWidget(sensitivity_label, 1, 0);
  grid->addWidget(m_ir_sensitivity, 1, 1);
  grid->addWidget(volume_label, 2, 0);
  grid->addWidget(m_speaker_volume, 2, 1);
  grid->addWidget(m_rumble, 3, 0, 1, -1);

  return box;
}

void WiiPane::WireUp()
{
  auto& settings = Settings::Instance();
  connect(&settings, &Settings::ConfigChanged, this, &WiiPane::RefreshPassthroughList);
  connect(m_passthrough_list, &QListWidget::itemClicked, this, &WiiPane::RefreshRemoveButton);
  connect(m_passthrough_add, &QPushButton::clicked, this, &WiiPane::AddPassthroughDevice);
  connect(m_passthrough_remove, &QPushButton::clicked, this, &WiiPane::RemovePassthroughDevice);

  connect(&settings, &Settings::EmulationStateChanged, this,
          [this](Core::State state) { LockForEmulation(state != Core::State::Uninitialized); });
}

void WiiPane::LockForEmulation(bool running)
{
  // These settings are only read when emulation starts (or would corrupt the running card).
  const std::array<QWidget*, 12> boot_only{m_screen_saver,
                                           m_pal60,
                                           m_language,
                                           m_widescreen,
                                           m_sound,
                                           m_sd_pack,
                                           m_sd_unpack,
                                           m_rumble,
                                           m_speaker_volume,
                                           m_ir_sensitivity,
                                           m_sensor_bar_position,
                                           m_wiilink};
  for (QWidget* widget : boot_only)
    widget->setEnabled(!running);
}

void WiiPane::RefreshRemoveButton()
{
  m_passthrough_remove->setEnabled(m_passthrough_list->currentIndex().isValid());
}

void WiiPane::RefreshPassthroughList()
{
  m_passthrough_list->clear();
  for (const USBUtils::DeviceInfo& device : Config::GetUSBDeviceWhitelist())
  {
    auto* entry =
        new QListWidgetItem(QString::fromStdString(device.ToDisplayString()), m_passthrough_list);
    entry->setData(Qt::UserRole, QVariant::fromValue(device));
  }
  RefreshRemoveButton();
}

void WiiPane::AddPassthroughDevice()
{
  auto allowed = Config::GetUSBDeviceWhitelist();
  const auto not_yet_allowed = [&allowed](const USBUtils::DeviceInfo& candidate) {
    return !allowed.contains(candidate);
  };

  const std::optional<USBUtils::DeviceInfo> picked =
      USBDevicePicker::Run(this, tr("Add New USB Device"), not_yet_allowed);
  if (!picked.has_value())
    return;

  if (allowed.contains(*picked))
  {
    ModalMessageBox::critical(this, tr("USB Whitelist Error"),
                              tr("This USB device is already whitelisted."));
    return;
  }

  allowed.emplace(*picked);
  Config::SetUSBDeviceWhitelist(allowed);
  RefreshPassthroughList();
}

void WiiPane::RemovePassthroughDevice()
{
  const QListWidgetItem* selected = m_passthrough_list->currentItem();
  if (selected == nullptr)
    return;

  const auto device = selected->data(Qt::UserRole).value<USBUtils::DeviceInfo>();
  auto allowed = Config::GetUSBDeviceWhitelist();
  allowed.erase(device);
  Config::SetUSBDeviceWhitelist(allowed);
  RefreshPassthroughList();
}

void WiiPane::PickSDImage()
{
  const QString current = QString::fromStdString(Config::Get(Config::MAIN_WII_SD_CARD_IMAGE_PATH));
  const QString chosen = DolphinFileDialog::getOpenFileName(
      this, tr("Select SD Card Image"), current, tr("SD Card Image (*.raw);;All Files (*)"));
  const QString native = QDir::toNativeSeparators(chosen);
  if (native.isEmpty())
    return;
  m_sd_image_path->SetTextAndUpdate(native);
}

void WiiPane::PickSDSyncFolder()
{
  const QString current = QString::fromStdString(File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX));
  const QString chosen = DolphinFileDialog::getExistingDirectory(
      this, tr("Select a Folder to Sync with the SD Card Image"), current);
  const QString native = QDir::toNativeSeparators(chosen);
  if (native.isEmpty())
    return;
  m_sd_folder_path->SetTextAndUpdate(native);
}

void WiiPane::ConvertSDCard(bool pack)
{
  const QString title = pack ? tr(Common::SD_PACK_TEXT) : tr(Common::SD_UNPACK_TEXT);
  // %1 is always the folder and %2 the image file.
  const QString question =
      pack ? tr("You are about to pack the content of the folder at %1 into the file at %2. "
                "All current content of the file will be deleted. Are you sure you want to "
                "continue?") :
             tr("You are about to unpack the content of the file at %2 into the folder at %1. "
                "All current content of the folder will be deleted. Are you sure you want to "
                "continue?");
  const QString folder = QString::fromStdString(File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX));
  const QString image = QString::fromStdString(File::GetUserPath(F_WIISDCARDIMAGE_IDX));

  const auto answer = ModalMessageBox::warning(this, title, question.arg(folder).arg(image),
                                               QMessageBox::Yes | QMessageBox::No);
  if (answer != QMessageBox::Yes)
    return;

  ParallelProgressDialog progress(tr("Converting..."), tr("Cancel"), 0, 0, this);
  QProgressDialog* raw = progress.GetRaw();
  raw->setWindowModality(Qt::WindowModal);
  raw->setWindowTitle(tr("Progress"));

  // The conversion runs on a worker thread while the dialog spins the event loop; the worker
  // closes the dialog when it is done.
  std::future<bool> worker = std::async(std::launch::async, [&progress, pack] {
    const auto cancelled = [&progress] { return progress.WasCanceled(); };
    const bool ok = pack ? Common::SyncSDFolderToSDImage(cancelled, false) :
                           Common::SyncSDImageToSDFolder(cancelled);
    progress.Reset();
    return ok;
  });
  raw->exec();

  if (!worker.get())
    ModalMessageBox::warning(this, pack ? tr(Common::SD_PACK_TEXT) : tr(Common::SD_UNPACK_TEXT),
                             tr("Conversion failed."));
}
