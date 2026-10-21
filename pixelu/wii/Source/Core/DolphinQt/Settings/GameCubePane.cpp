// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/GameCubePane.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <array>
#include <optional>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "DolphinQt/Config/SettingsWindow.h"
#ifdef HAS_LIBMGBA
#include "Core/NetPlayServer.h"
#endif
#include "Core/System.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#ifdef HAS_LIBMGBA
#include "DolphinQt/Config/ConfigControls/ConfigText.h"
#include "DolphinQt/Config/ConfigControls/ConfigUserPath.h"
#endif
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/GCMemcardManager.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Settings/BroadbandAdapterSettingsDialog.h"
#include "DolphinQt/Settings/TriforcePane.h"

namespace
{
using ExpansionInterface::EXIDeviceType;
using ExpansionInterface::Slot;

// Slots that get a device selector on this page, top to bottom.
constexpr std::initializer_list<Slot> SELECTABLE_SLOTS = {Slot::A, Slot::B, Slot::SP1};

// The same file/folder name, spelled for each region (index order: J, U, PAL, DEV).
struct RegionVariants
{
  std::string jp, us, eu, dev;

  bool Contains(const std::string& candidate) const
  {
    return candidate == jp || candidate == us || candidate == eu || candidate == dev;
  }
};

template <typename PathFor>
RegionVariants VariantsOf(PathFor&& path_for)
{
  RegionVariants variants;
  variants.jp = path_for(DiscIO::Region::NTSC_J);
  variants.us = path_for(DiscIO::Region::NTSC_U);
  variants.eu = path_for(DiscIO::Region::PAL);
  variants.dev = path_for(DiscIO::Region::DEV);
  return variants;
}

QString FileNameOf(const std::string& path)
{
  return QString::fromStdString(PathToFileName(path));
}

EXIDeviceType SelectedDevice(const QComboBox* combo)
{
  return static_cast<EXIDeviceType>(combo->currentData().toInt());
}

// Which broadband adapter dialog (if any) configures a device.
std::optional<BroadbandAdapterSettingsDialog::Type> NetworkDialogFor(EXIDeviceType device)
{
  using Kind = BroadbandAdapterSettingsDialog::Type;
  switch (device)
  {
  case EXIDeviceType::Ethernet:
    return Kind::Ethernet;
  case EXIDeviceType::EthernetXLink:
    return Kind::XLinkKai;
  case EXIDeviceType::EthernetTapServer:
    return Kind::TapServer;
  case EXIDeviceType::ModemTapServer:
    return Kind::ModemTapServer;
  case EXIDeviceType::EthernetBuiltIn:
    return Kind::BuiltIn;
  default:
    return std::nullopt;
  }
}

// Tells a running game that the device in |slot| was swapped (it's unplugged for a second).
void ReplugIfRunning(Slot slot, EXIDeviceType device, bool changed)
{
  auto& system = Core::System::GetInstance();
  if (Core::IsRunning(system) && changed)
    system.GetExpansionInterface().ChangeDevice(slot, device);
}
}  // namespace

GameCubePane::GameCubePane(MainWindow* main_window)
{
  auto* const page = new QVBoxLayout(this);
  QWidget* const ipl = BuildIplBox();
  QWidget* const devices = BuildDeviceBox();
#ifdef HAS_LIBMGBA
  QWidget* const gba = BuildGbaBox();
#endif
  page->addWidget(ipl);
  page->addWidget(devices);
#ifdef HAS_LIBMGBA
  page->addWidget(gba);
#endif
  page->addStretch();

  ReadConfig();
  WireUp();

  connect(this, &GameCubePane::ShowTriforceWindow, main_window, &MainWindow::ShowTriforceWindow);
}

QWidget* GameCubePane::BuildIplBox()
{
  auto* const box = new QGroupBox(tr("IPL Settings"), this);
  auto* const column = new QVBoxLayout(box);

  m_skip_ipl = new ConfigBool(tr("Skip Main Menu"), Config::MAIN_SKIP_IPL);
  column->addWidget(m_skip_ipl);

  auto* const form = new QFormLayout;
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  column->addLayout(form);

  // Order matches the stored language index.
  const QStringList languages{tr("English"), tr("German"),  tr("French"),
                              tr("Spanish"), tr("Italian"), tr("Dutch")};
  m_ipl_language = new ConfigChoice(languages, Config::MAIN_GC_LANGUAGE);
  form->addRow(tr("System Language:"), m_ipl_language);

  return box;
}

void GameCubePane::BuildCardSlotPaths(Slot slot)
{
  CardSlotPaths& paths = m_paths[slot];

  m_memcard_rows[slot] = new QHBoxLayout();
  paths.memcard_label = new QLabel(tr("Memory Card Path:"));
  paths.memcard = new QLineEdit();
  m_memcard_rows[slot]->addWidget(paths.memcard_label);
  m_memcard_rows[slot]->addWidget(paths.memcard);

  m_cartridge_rows[slot] = new QHBoxLayout();
  paths.cartridge_label = new QLabel(tr("GBA Cartridge Path:"));
  paths.cartridge = new QLineEdit();
  m_cartridge_rows[slot]->addWidget(paths.cartridge_label);
  m_cartridge_rows[slot]->addWidget(paths.cartridge);

  m_gci_rows[slot] = new QVBoxLayout();
  paths.gci_label = new QLabel(tr("GCI Folder Path:"));
  paths.gci_override_warning =
      new QLabel(tr("Warning: A GCI folder override path is currently configured for this "
                    "slot. Adjusting the GCI path here will have no effect."));
  paths.gci_override_warning->setHidden(true);
  paths.gci_override_warning->setWordWrap(true);
  paths.gci = new QLineEdit();

  auto* const gci_row = new QHBoxLayout();
  gci_row->addWidget(paths.gci_label);
  gci_row->addWidget(paths.gci);
  m_gci_rows[slot]->addWidget(paths.gci_override_warning);
  m_gci_rows[slot]->addLayout(gci_row);
}

void GameCubePane::FillDeviceCombos()
{
  // Device names come from the EXI device formatter and are translated here.
  const auto display_name = [](EXIDeviceType device) {
    return tr(fmt::format("{:n}", device).c_str());
  };

  for (const EXIDeviceType device :
       {EXIDeviceType::None, EXIDeviceType::Dummy, EXIDeviceType::MemoryCard,
        EXIDeviceType::MemoryCardFolder, EXIDeviceType::Gecko, EXIDeviceType::AGP,
        EXIDeviceType::Microphone})
  {
    const QString name = display_name(device);
    for (const Slot slot : ExpansionInterface::MEMCARD_SLOTS)
      m_device_combos[slot]->addItem(name, static_cast<int>(device));
  }

  static constexpr EXIDeviceType serial_port_devices[] = {
      EXIDeviceType::None,
      EXIDeviceType::Dummy,
      EXIDeviceType::Ethernet,
      EXIDeviceType::EthernetXLink,
      EXIDeviceType::EthernetTapServer,
      EXIDeviceType::EthernetBuiltIn,
#ifdef HAVE_CPPIPC
      EXIDeviceType::EthernetIPC,
#endif
      EXIDeviceType::ModemTapServer,
      EXIDeviceType::Baseboard,
  };
  for (const EXIDeviceType device : serial_port_devices)
    m_device_combos[Slot::SP1]->addItem(display_name(device), static_cast<int>(device));
}

QWidget* GameCubePane::BuildDeviceBox()
{
  auto* const box = new QGroupBox(tr("Device Settings"), this);
  auto* const grid = new QGridLayout(box);

  for (const Slot slot : SELECTABLE_SLOTS)
  {
    m_device_combos[slot] = new QComboBox(box);
    m_device_combos[slot]->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_configure_buttons[slot] = new NonDefaultQPushButton(tr("..."), box);
    m_configure_buttons[slot]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }

  for (const Slot slot : ExpansionInterface::MEMCARD_SLOTS)
    BuildCardSlotPaths(slot);

  FillDeviceCombos();

  // Each slot gets a selector row; A and B are followed by their three path rows.
  int row = 0;
  const auto add_selector = [&](const QString& title, Slot slot) {
    grid->addWidget(new QLabel(title), row, 0);
    grid->addWidget(m_device_combos[slot], row, 1);
    grid->addWidget(m_configure_buttons[slot], row, 2);
    ++row;
  };
  const auto add_paths = [&](Slot slot) {
    for (QLayout* const line :
         {static_cast<QLayout*>(m_memcard_rows[slot]),
          static_cast<QLayout*>(m_cartridge_rows[slot]), static_cast<QLayout*>(m_gci_rows[slot])})
    {
      grid->addLayout(line, row, 0, 1, 3);
      ++row;
    }
  };

  add_selector(tr("Slot A:"), Slot::A);
  add_paths(Slot::A);
  add_selector(tr("Slot B:"), Slot::B);
  add_paths(Slot::B);
  add_selector(tr("SP1:"), Slot::SP1);

  return box;
}

#ifdef HAS_LIBMGBA
QWidget* GameCubePane::BuildGbaBox()
{
  auto* const box = new QGroupBox(tr("GBA Settings"), this);
  auto* const grid = new QGridLayout(box);
  int row = 0;

  const auto add_path_row = [&](const QString& title, QWidget* edit, QPushButton* browse) {
    grid->addWidget(new QLabel(title), row, 0);
    grid->addWidget(edit, row, 1);
    grid->addWidget(browse, row, 2);
    ++row;
  };

  m_gba_bios = new ConfigUserPath(F_GBABIOS_IDX, Config::MAIN_GBA_BIOS_PATH);
  m_gba_bios_browse = new NonDefaultQPushButton(QStringLiteral("..."));
  add_path_row(tr("BIOS:"), m_gba_bios, m_gba_bios_browse);

  for (size_t port = 0; port < m_gba_roms.size(); ++port)
  {
    m_gba_roms[port] = new ConfigText(Config::MAIN_GBA_ROM_PATHS[port]);
    m_gba_rom_browse[port] = new NonDefaultQPushButton(QStringLiteral("..."));
    const QString title = port == Config::GBPLAYER_GBA_INDEX ? tr("Game Boy Player ROM:") :
                                                               tr("Port %1 ROM:").arg(port + 1);
    add_path_row(title, m_gba_roms[port], m_gba_rom_browse[port]);
  }

  m_gba_saves_beside_rom =
      new ConfigBool(tr("Save in Same Directory as the ROM"), Config::MAIN_GBA_SAVES_IN_ROM_PATH);
  grid->addWidget(m_gba_saves_beside_rom, row, 0, 1, -1);
  ++row;

  m_gba_saves = new ConfigUserPath(D_GBASAVES_IDX, Config::MAIN_GBA_SAVES_PATH);
  m_gba_saves_browse = new NonDefaultQPushButton(QStringLiteral("..."));
  add_path_row(tr("Saves:"), m_gba_saves, m_gba_saves_browse);

  return box;
}
#endif

void GameCubePane::WireUp()
{
  for (const Slot slot : SELECTABLE_SLOTS)
  {
    QComboBox* const combo = m_device_combos[slot];
    connect(combo, &QComboBox::currentIndexChanged, this, [this, slot] { RefreshSlot(slot); });
    connect(combo, &QComboBox::currentIndexChanged, this, &GameCubePane::WriteDevices);
    connect(m_configure_buttons[slot], &QPushButton::clicked, this,
            [this, slot] { ConfigureSlot(slot); });
  }

  for (const Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    const CardSlotPaths& paths = m_paths[slot];
    // A rejected memory card or GCI path puts the previous value back.
    connect(paths.memcard, &QLineEdit::editingFinished, this, [this, slot] {
      if (!ApplyMemcard(slot, m_paths[slot].memcard->text()))
        ReadConfig();
    });
    connect(paths.cartridge, &QLineEdit::editingFinished, this,
            [this, slot] { ApplyCartridge(slot, m_paths[slot].cartridge->text()); });
    connect(paths.gci, &QLineEdit::editingFinished, this, [this, slot] {
      if (!ApplyGCIFolder(slot, m_paths[slot].gci->text()))
        ReadConfig();
    });
  }

#ifdef HAS_LIBMGBA
  connect(m_gba_bios_browse, &QPushButton::clicked, this, &GameCubePane::PickGBABios);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(m_gba_saves_beside_rom, &QCheckBox::checkStateChanged, this,
          &GameCubePane::OnSavesBesideRomToggled);
#else
  connect(m_gba_saves_beside_rom, &QCheckBox::stateChanged, this,
          &GameCubePane::OnSavesBesideRomToggled);
#endif
  connect(m_gba_saves_browse, &QPushButton::clicked, this, &GameCubePane::PickGBASaves);
  for (size_t port = 0; port < m_gba_rom_browse.size(); ++port)
  {
    connect(m_gba_rom_browse[port], &QPushButton::clicked, this,
            [this, port] { PickGBARom(port); });
  }
#endif

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &GameCubePane::OnEmulationStateChanged);
  OnEmulationStateChanged();
}

void GameCubePane::OnEmulationStateChanged()
{
#ifdef HAS_LIBMGBA
  // GBA files can't be changed during NetPlay.
  const bool editable = !NetPlay::IsNetPlayRunning();
  for (QWidget* const widget : std::initializer_list<QWidget*>{
           m_gba_bios, m_gba_bios_browse, m_gba_saves_beside_rom, m_gba_saves, m_gba_saves_browse})
  {
    widget->setEnabled(editable);
  }
  for (size_t port = 0; port < m_gba_rom_browse.size(); ++port)
  {
    m_gba_roms[port]->setEnabled(editable);
    m_gba_rom_browse[port]->setEnabled(editable);
  }
#endif
}

void GameCubePane::RefreshSlot(Slot slot)
{
  const EXIDeviceType device = SelectedDevice(m_device_combos[slot]);
  bool configurable = false;

  if (slot == Slot::A || slot == Slot::B)
  {
    configurable = device == EXIDeviceType::MemoryCard ||
                   device == EXIDeviceType::MemoryCardFolder || device == EXIDeviceType::AGP ||
                   device == EXIDeviceType::Microphone;

    // A path editor is only offered when its device is selected and the slot doesn't use the
    // default location.
    const bool memcard_shown =
        device == EXIDeviceType::MemoryCard && !Config::IsDefaultMemcardPathConfigured(slot);
    const bool gci_shown = device == EXIDeviceType::MemoryCardFolder &&
                           !Config::IsDefaultGCIFolderPathConfigured(slot);
    const bool cartridge_shown = device == EXIDeviceType::AGP;

    CardSlotPaths& paths = m_paths[slot];
    paths.memcard_label->setHidden(!memcard_shown);
    paths.memcard->setHidden(!memcard_shown);
    paths.cartridge_label->setHidden(!cartridge_shown);
    paths.cartridge->setHidden(!cartridge_shown);
    paths.gci_label->setHidden(!gci_shown);
    paths.gci->setHidden(!gci_shown);

    // Older setups often used the GCI override path instead of the per-slot folder setting;
    // warn that editing the folder here then does nothing.
    const bool overridden = device == EXIDeviceType::MemoryCardFolder &&
                            !Config::Get(Config::GetInfoForGCIPathOverride(slot)).empty();
    paths.gci_override_warning->setHidden(!overridden);
  }
  else if (slot == Slot::SP1)
  {
    configurable = NetworkDialogFor(device).has_value() || device == EXIDeviceType::Baseboard;
  }

  m_configure_buttons[slot]->setEnabled(configurable);
}

void GameCubePane::ConfigureSlot(Slot slot)
{
  const EXIDeviceType device = SelectedDevice(m_device_combos[slot]);

  if (const auto network = NetworkDialogFor(device))
  {
    BroadbandAdapterSettingsDialog dialog(this, *network);
    dialog.exec();
    return;
  }

  switch (device)
  {
  case EXIDeviceType::MemoryCard:
    PickMemcard(slot);
    break;
  case EXIDeviceType::MemoryCardFolder:
    PickGCIFolder(slot);
    break;
  case EXIDeviceType::AGP:
    PickCartridge(slot);
    break;
  case EXIDeviceType::Microphone:
  {
    MappingWindow mapping(this, MappingWindow::Type::MAPPING_GC_MICROPHONE, static_cast<int>(slot));
    mapping.exec();
    break;
  }
  case EXIDeviceType::Baseboard:
    emit ShowTriforceWindow();
    break;
  default:
    PanicAlertFmt("Unknown settings pressed for {}", device);
    break;
  }
}

void GameCubePane::PickMemcard(Slot slot)
{
  ASSERT(ExpansionInterface::IsMemcardSlot(slot));

  const QString start = QString::fromStdString(File::GetUserPath(D_GCUSER_IDX));
  const QString chosen = DolphinFileDialog::getSaveFileName(
      this, tr("Choose a File to Open or Create"), start, tr("GameCube Memory Cards (*.raw *.gcp)"),
      nullptr, QFileDialog::DontConfirmOverwrite);
  if (!chosen.isEmpty())
    ApplyMemcard(slot, chosen);
}

bool GameCubePane::ApplyMemcard(Slot slot, const QString& filename)
{
  if (filename.isEmpty())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Cannot set memory card to an empty path."));
    return false;
  }

  const std::string chosen =
      WithUnifiedPathSeparators(QFileInfo(filename).absoluteFilePath().toStdString());

  // The file name has to carry one of the region suffixes.
  const RegionVariants variants = VariantsOf(
      [&](DiscIO::Region region) { return Config::GetMemcardPath(chosen, slot, region); });
  if (!variants.Contains(chosen))
  {
    // TODO: We could try to autodetect the card region here and offer automatic renaming.
    ModalMessageBox::critical(
        this, tr("Error"),
        tr("The filename %1 does not conform to Dolphin's region code format for memory "
           "cards. Please rename this file to either %2, %3, %4, or %5, matching the region "
           "of the save files that are on it.")
            .arg(FileNameOf(chosen))
            .arg(FileNameOf(variants.us))
            .arg(FileNameOf(variants.eu))
            .arg(FileNameOf(variants.jp))
            .arg(FileNameOf(variants.dev)));
    return false;
  }

  // Every existing regional card must be a readable memory card.
  for (const std::string& card : {variants.jp, variants.us, variants.eu})
  {
    if (!File::Exists(card))
      continue;

    auto [error_code, opened] = Memcard::GCMemcard::Open(card);
    const bool usable = !error_code.HasCriticalErrors() && opened && opened->IsValid();
    if (!usable)
    {
      ModalMessageBox::critical(
          this, tr("Error"),
          tr("The file\n%1\nis either corrupted or not a GameCube memory card file.\n%2")
              .arg(QString::fromStdString(card))
              .arg(GCMemcardManager::GetErrorMessagesForErrorCode(error_code)));
      return false;
    }
  }

  // Both slots can't share a card. Comparing the PAL names is enough: all regional names
  // derive from the same base.
  for (const Slot other : ExpansionInterface::MEMCARD_SLOTS)
  {
    if (other != slot && variants.eu == Config::GetMemcardPath(other, DiscIO::Region::PAL))
    {
      ModalMessageBox::critical(
          this, tr("Error"),
          tr("The same file can't be used in multiple slots; it is already used by %1.")
              .arg(QString::fromStdString(fmt::to_string(other))));
      return false;
    }
  }

  const std::string previous_eu = Config::GetMemcardPath(slot, DiscIO::Region::PAL);
  Config::SetBase(Config::GetInfoForMemcardPath(slot), chosen);

  // TODO: This should probably be done by a config change callback instead.
  ReplugIfRunning(slot, EXIDeviceType::MemoryCard, variants.eu != previous_eu);

  ReadConfig();
  return true;
}

void GameCubePane::PickGCIFolder(Slot slot)
{
  ASSERT(ExpansionInterface::IsMemcardSlot(slot));

  const QString chosen = DolphinFileDialog::getExistingDirectory(
      this, tr("Choose GCI Base Folder"), QString::fromStdString(File::GetUserPath(D_GCUSER_IDX)));
  if (!chosen.isEmpty())
    ApplyGCIFolder(slot, chosen);
}

bool GameCubePane::ApplyGCIFolder(Slot slot, const QString& path)
{
  if (path.isEmpty())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Cannot set GCI folder to an empty path."));
    return false;
  }

  std::string chosen = WithUnifiedPathSeparators(QFileInfo(path).absoluteFilePath().toStdString());
  while (chosen.ends_with('/'))
    chosen.pop_back();

  const RegionVariants defaults =
      VariantsOf([&](DiscIO::Region region) { return Config::GetGCIFolderPath("", slot, region); });

  bool changed;
  if (defaults.Contains(chosen))
  {
    // Picking a default folder resets the setting. (Two slots can never share a default
    // folder, so no clash check is needed.)
    chosen.clear();
    changed = !Config::IsDefaultGCIFolderPathConfigured(slot);
  }
  else
  {
    // The folder name has to carry one of the region suffixes.
    const RegionVariants variants = VariantsOf(
        [&](DiscIO::Region region) { return Config::GetGCIFolderPath(chosen, slot, region); });
    if (!variants.Contains(chosen))
    {
      // TODO: We could try to autodetect the card region here and offer automatic renaming.
      ModalMessageBox::critical(
          this, tr("Error"),
          tr("The folder %1 does not conform to Dolphin's region code format for GCI "
             "folders. Please rename this folder to either %2, %3, %4, or %5, matching the "
             "region of the save files that are in it.")
              .arg(FileNameOf(chosen))
              .arg(FileNameOf(variants.us))
              .arg(FileNameOf(variants.eu))
              .arg(FileNameOf(variants.jp))
              .arg(FileNameOf(variants.dev)));
      return false;
    }

    // Both slots can't share a folder (PAL names are compared, as for memory cards).
    for (const Slot other : ExpansionInterface::MEMCARD_SLOTS)
    {
      if (other != slot && variants.eu == Config::GetGCIFolderPath(other, DiscIO::Region::PAL))
      {
        ModalMessageBox::critical(
            this, tr("Error"),
            tr("The same folder can't be used in multiple slots; it is already used by %1.")
                .arg(QString::fromStdString(fmt::to_string(other))));
        return false;
      }
    }

    changed = variants.eu != Config::GetGCIFolderPath(slot, DiscIO::Region::PAL);
  }

  Config::SetBase(Config::GetInfoForGCIPath(slot), chosen);

  // TODO: This should probably be done by a config change callback instead.
  ReplugIfRunning(slot, EXIDeviceType::MemoryCardFolder, changed);

  ReadConfig();
  return true;
}

void GameCubePane::PickCartridge(Slot slot)
{
  ASSERT(ExpansionInterface::IsMemcardSlot(slot));

  const QString chosen = DolphinFileDialog::getSaveFileName(
      this, tr("Choose a File to Open"), QString::fromStdString(File::GetUserPath(D_GCUSER_IDX)),
      tr("Game Boy Advance Carts (*.gba)"), nullptr, QFileDialog::DontConfirmOverwrite);
  if (!chosen.isEmpty())
    ApplyCartridge(slot, chosen);
}

void GameCubePane::ApplyCartridge(Slot slot, const QString& filename)
{
  const auto& setting = Config::GetInfoForAGPCartPath(slot);

  QString absolute;
  if (!filename.isEmpty())
    absolute = QFileInfo(filename).absoluteFilePath();
  const QString previous =
      QFileInfo(QString::fromStdString(Config::Get(setting))).absoluteFilePath();

  Config::SetBase(setting, absolute.toStdString());

  // Like memory cards, a swapped cartridge is announced by briefly unplugging the adapter.
  auto& system = Core::System::GetInstance();
  if (Core::IsRunning(system) && absolute != previous)
    system.GetExpansionInterface().ChangeDevice(slot, EXIDeviceType::AGP);

  ReadConfig();
}

#ifdef HAS_LIBMGBA

void GameCubePane::PickGBABios()
{
  const QString current = QString::fromStdString(Config::Get(Config::MAIN_GBA_BIOS_PATH));
  const QString chosen = QDir::toNativeSeparators(DolphinFileDialog::getOpenFileName(
      this, tr("Select GBA BIOS"), current, tr("All Files (*)")));
  if (!chosen.isEmpty())
    m_gba_bios->SetTextAndUpdate(chosen);
}

void GameCubePane::PickGBARom(size_t index)
{
  const QString chosen = QString::fromStdString(GetOpenGBARom({}));
  if (!chosen.isEmpty())
    m_gba_roms[index]->SetTextAndUpdate(chosen);
}

void GameCubePane::OnSavesBesideRomToggled()
{
  const bool own_folder = !m_gba_saves_beside_rom->isChecked();
  m_gba_saves->setEnabled(own_folder);
  m_gba_saves_browse->setEnabled(own_folder);
}

void GameCubePane::PickGBASaves()
{
  const QString current = QString::fromStdString(Config::Get(Config::MAIN_GBA_SAVES_PATH));
  const QString chosen = QDir::toNativeSeparators(
      DolphinFileDialog::getExistingDirectory(this, tr("Select GBA Saves Path"), current));
  if (chosen.isEmpty())
    return;

  m_gba_saves->setText(chosen);
  WriteDevices();
}

#endif  // HAS_LIBMGBA

void GameCubePane::ReadConfig()
{
  // Without an IPL dump there is no menu to show, so a ticked "Skip Main Menu" is locked.
  bool have_ipl = false;
  for (const std::string region_dir : {USA_DIR, JAP_DIR, EUR_DIR})
  {
    const std::string relative = DIR_SEP + region_dir + DIR_SEP GC_IPL;
    have_ipl = File::Exists(File::GetUserPath(D_GCUSER_IDX) + relative) ||
               File::Exists(File::GetSysDirectory() + GC_SYS_DIR + relative);
    if (have_ipl)
      break;
  }
  m_skip_ipl->setEnabled(have_ipl || !m_skip_ipl->isChecked());
  m_skip_ipl->SetDescription(have_ipl ? QString{} : tr("Put IPL ROMs in User/GC/<region>."));

  for (const Slot slot : SELECTABLE_SLOTS)
  {
    const EXIDeviceType device = Config::Get(Config::GetInfoForEXIDevice(slot));
    QComboBox* const combo = m_device_combos[slot];
    SignalBlocking(combo)->setCurrentIndex(combo->findData(static_cast<int>(device)));
    RefreshSlot(slot);
  }

  for (const Slot slot : ExpansionInterface::MEMCARD_SLOTS)
  {
    const CardSlotPaths& paths = m_paths[slot];
    const std::string memcard = Config::GetMemcardPath(slot, std::nullopt);
    SignalBlocking(paths.memcard)->setText(QString::fromStdString(memcard));
    const std::string cartridge = Config::Get(Config::GetInfoForAGPCartPath(slot));
    SignalBlocking(paths.cartridge)->setText(QString::fromStdString(cartridge));
    const std::string gci = Config::GetGCIFolderPath(slot, std::nullopt);
    SignalBlocking(paths.gci)->setText(QString::fromStdString(gci));
  }
}

void GameCubePane::WriteDevices()
{
  Config::ConfigChangeCallbackGuard batch;

  auto& system = Core::System::GetInstance();
  for (const Slot slot : SELECTABLE_SLOTS)
  {
    const auto& setting = Config::GetInfoForEXIDevice(slot);
    const EXIDeviceType wanted = SelectedDevice(m_device_combos[slot]);
    const EXIDeviceType current = Config::Get(setting);

    // A running game sees the swap immediately.
    if (Core::IsRunning(system) && current != wanted)
      system.GetExpansionInterface().ChangeDevice(slot, wanted);

    Config::SetBaseOrCurrent(setting, wanted);
  }

#ifdef HAS_LIBMGBA
  // Let a hosted NetPlay session pick up the GBA settings.
  if (!NetPlay::IsNetPlayRunning())
  {
    if (auto server = Settings::Instance().GetNetPlayServer())
      server->SetGBAConfig(server->GetGBAConfig(), true);
  }
#endif

  ReadConfig();
}

std::string GameCubePane::GetOpenGBARom(std::string_view title)
{
  QString caption = tr("Select GBA ROM");
  if (!title.empty())
    caption += QStringLiteral(": %1").arg(QString::fromStdString(std::string(title)));

  const QString chosen = DolphinFileDialog::getOpenFileName(
      nullptr, caption, QString(),
      tr("Game Boy Advance ROMs (*.gba *.gbc *.gb *.7z *.zip *.agb *.mb *.rom *.bin);;All Files "
         "(*)"));
  return QDir::toNativeSeparators(chosen).toStdString();
}
