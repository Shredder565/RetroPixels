// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Boot/Boot.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/DolReader.h"
#include "Core/Boot/ElfReader.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "Core/NetPlayProto.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"
#include "DiscIO/GameModDescriptor.h"
#include "DiscIO/RiivolutionParser.h"
#include "DiscIO/RiivolutionPatcher.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWad.h"

namespace
{
// Splits an M3U playlist into the paths it lists, each resolved relative to
// the playlist's own folder. All listed files must exist.
std::vector<std::string> ReadM3UFile(const std::string& m3u_path, const std::string& folder_path)
{
  // U+FEFF encoded as UTF-8.
  constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";

  std::vector<std::string> found;
  std::vector<std::string> missing;

  std::ifstream playlist;
  File::OpenFStream(playlist, m3u_path, std::ios_base::in);

  for (std::string line; std::getline(playlist, line);)
  {
#ifndef _WIN32
    // Tolerate CRLF line endings on systems where getline keeps the CR.
    if (line.ends_with('\r'))
      line.pop_back();
#endif
    if (line.starts_with(byte_order_mark))
    {
      WARN_LOG_FMT(BOOT, "UTF-8 BOM in file: {}", m3u_path);
      line.erase(0, byte_order_mark.size());
    }

    // Blank lines and '#' comments carry no path.
    if (line.empty() || line.front() == '#')
      continue;

    std::string resolved = PathToString(StringToPath(folder_path) / StringToPath(line));
    auto& bucket = File::Exists(resolved) ? found : missing;
    bucket.push_back(std::move(resolved));
  }

  if (!missing.empty())
  {
    PanicAlertFmtT("Files specified in the M3U file \"{0}\" were not found:\n{1}", m3u_path,
                   fmt::join(missing, "\n"));
    return {};
  }

  if (found.empty())
    PanicAlertFmtT("No paths found in the M3U file \"{0}\"", m3u_path);

  return found;
}

// Lower-cased extension of `path`; optionally also its folder.
std::string LowerExtension(const std::string& path, std::string* folder = nullptr)
{
  std::string extension;
  SplitPath(path, folder, nullptr, &extension);
  Common::ToLower(&extension);
  return extension;
}

bool IsDiscOrExecutableExtension(const std::string& extension)
{
  static const std::unordered_set<std::string> extensions = {".gcm",  ".bin",  ".iso", ".tgc",
                                                             ".wbfs", ".ciso", ".gcz", ".wia",
                                                             ".rvz",  ".nfs",  ".dol", ".elf"};
  return extensions.contains(extension);
}

// Puts `disc` into the emulated drive. The returned pointer is only safe to
// use during boot: afterwards the DVD thread owns the volume.
const DiscIO::VolumeDisc* SetDisc(DVD::DVDInterface& dvd_interface,
                                  std::unique_ptr<DiscIO::VolumeDisc> disc,
                                  std::vector<std::string> auto_disc_change_paths = {})
{
  const DiscIO::VolumeDisc* const inserted = disc.get();
  dvd_interface.SetDisc(std::move(disc), auto_disc_change_paths);
  return inserted;
}

// Inserts the user's configured default disc, if any, for titles that don't
// come from a disc themselves.
void SetDefaultDisc(DVD::DVDInterface& dvd_interface)
{
  const std::string default_iso = Config::Get(Config::MAIN_DEFAULT_ISO);
  if (default_iso.empty())
    return;
  SetDisc(dvd_interface, DiscIO::CreateDiscForCore(default_iso));
}

// Executables are started without the IPL, which would normally leave a
// handler at every exception vector. Plant an `rfi` at each one instead.
void CopyDefaultExceptionHandlers(Core::System& system)
{
  constexpr u32 RFI = 0x4C000064;
  constexpr std::array<u32, 15> vectors = {0x0100, 0x0200, 0x0300, 0x0400, 0x0500,
                                           0x0600, 0x0700, 0x0800, 0x0900, 0x0C00,
                                           0x0D00, 0x0F00, 0x1300, 0x1400, 0x1700};
  auto& memory = system.GetMemory();
  for (const u32 vector : vectors)
    memory.Write_U32(RFI, vector);
}

// Bookkeeping shared by every successful boot path.
void FinishTitleBoot(const Core::CPUThreadGuard& guard, const DiscIO::Volume* volume)
{
  AchievementManager::GetInstance().LoadGame(volume);
  SConfig::OnTitleDirectlyBooted(guard);
}

// Wii discs, WADs and NAND titles are encrypted with keys that only the owner's own console has
// (RetroPixels has none). Without its keys file such a title is refused with one clear message
// before anything is set up. GameCube games and Wii homebrew executables need no keys.
bool HaveWiiKeys(IOS::HLE::IOSC::ConsoleType console_type)
{
  if (IOS::HLE::IOSC::HasConsoleKeys(console_type))
    return true;
  IOS::HLE::IOSC::ReportMissingKeys(console_type);
  return false;
}

bool HaveWiiKeys(const DiscIO::Volume& volume)
{
  return HaveWiiKeys(volume.GetTicket(volume.GetGamePartition()).GetConsoleType());
}
}  // namespace

// BootSessionData -----------------------------------------------------------------------------

BootSessionData::BootSessionData() = default;
BootSessionData::BootSessionData(BootSessionData&& other) = default;
BootSessionData& BootSessionData::operator=(BootSessionData&& other) = default;
BootSessionData::~BootSessionData() = default;

BootSessionData::BootSessionData(std::optional<std::string> savestate_path,
                                 DeleteSavestateAfterBoot delete_savestate)
    : m_savestate_path(std::move(savestate_path)), m_delete_savestate(delete_savestate)
{
}

void BootSessionData::SetNetplaySettings(std::unique_ptr<NetPlay::NetSettings> netplay_settings)
{
  m_netplay_settings = std::move(netplay_settings);
}

const NetPlay::NetSettings* BootSessionData::GetNetplaySettings() const
{
  return m_netplay_settings.get();
}

void BootSessionData::SetSavestateData(std::optional<std::string> savestate_path,
                                       DeleteSavestateAfterBoot delete_savestate)
{
  m_delete_savestate = delete_savestate;
  m_savestate_path = std::move(savestate_path);
}

DeleteSavestateAfterBoot BootSessionData::GetDeleteSavestate() const
{
  return m_delete_savestate;
}

const std::optional<std::string>& BootSessionData::GetSavestatePath() const
{
  return m_savestate_path;
}

void BootSessionData::SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs,
                                     std::vector<u64> titles, std::string redirect_folder,
                                     WiiSyncCleanupFunction cleanup)
{
  m_wii_sync_cleanup = std::move(cleanup);
  m_wii_sync_redirect_folder = std::move(redirect_folder);
  m_wii_sync_titles = std::move(titles);
  m_wii_sync_fs = std::move(fs);
}

void BootSessionData::InvokeWiiSyncCleanup() const
{
  if (m_wii_sync_cleanup)
    m_wii_sync_cleanup();
}

IOS::HLE::FS::FileSystem* BootSessionData::GetWiiSyncFS() const
{
  return m_wii_sync_fs.get();
}

const std::string& BootSessionData::GetWiiSyncRedirectFolder() const
{
  return m_wii_sync_redirect_folder;
}

const std::vector<u64>& BootSessionData::GetWiiSyncTitles() const
{
  return m_wii_sync_titles;
}

// BootParameters ------------------------------------------------------------------------------

BootParameters::BootParameters(Parameters&& parameters_, BootSessionData boot_session_data_)
    : parameters(std::move(parameters_)), boot_session_data(std::move(boot_session_data_))
{
}

std::unique_ptr<BootParameters> BootParameters::GenerateFromFile(std::string boot_path,
                                                                 BootSessionData boot_session_data_)
{
  std::vector<std::string> single;
  single.push_back(std::move(boot_path));
  return GenerateFromFile(std::move(single), std::move(boot_session_data_));
}

std::unique_ptr<BootParameters> BootParameters::GenerateFromFile(std::vector<std::string> paths,
                                                                 BootSessionData boot_session_data_)
{
  ASSERT(!paths.empty());
  const auto unify_all = [&paths] {
    for (std::string& entry : paths)
      UnifyPathSeparators(entry);
  };
  unify_all();

  // A bad path typically comes from the command line.
  if (!File::Exists(paths.front()))
  {
    PanicAlertFmtT("The specified file \"{0}\" does not exist", paths.front());
    return {};
  }

  std::string folder_path;
  std::string extension = LowerExtension(paths.front(), &folder_path);

  // A playlist is replaced by the files it lists; the first one boots.
  if (extension == ".m3u" || extension == ".m3u8")
  {
    paths = ReadM3UFile(paths.front(), folder_path);
    if (paths.empty())
      return {};
    unify_all();
    extension = LowerExtension(paths.front());
  }

  std::string path = paths.front();
  // The remaining list is only kept for multi-disc games.
  if (paths.size() == 1)
    paths.clear();

#ifdef ANDROID
  if (extension.empty() && IsPathAndroidContent(path))
    extension = LowerExtension(GetAndroidContentDisplayName(path));
#endif

  const auto make = [&boot_session_data_](Parameters&& what) {
    return std::make_unique<BootParameters>(std::move(what), std::move(boot_session_data_));
  };

  if (IsDiscOrExecutableExtension(extension))
  {
    // DOL and ELF files may still be wrapped as a disc by the disc layer, so
    // that is tried first for every extension in this group.
    if (std::unique_ptr<DiscIO::VolumeDisc> disc = DiscIO::CreateDiscForCore(path))
      return make(Disc{std::move(path), std::move(disc), paths});

    if (extension == ".elf")
    {
      std::unique_ptr<BootExecutableReader> reader = std::make_unique<ElfReader>(path);
      return make(Executable{std::move(path), std::move(reader)});
    }
    if (extension == ".dol")
    {
      std::unique_ptr<BootExecutableReader> reader = std::make_unique<DolReader>(path);
      return make(Executable{std::move(path), std::move(reader)});
    }

    PanicAlertFmtT("\"{0}\" is an invalid GCM/ISO file, or is not a GC/Wii ISO.", path);
    return {};
  }

  if (extension == ".dff")
    return make(DFF{std::move(path)});

  if (extension == ".wad")
  {
    if (std::unique_ptr<DiscIO::VolumeWAD> wad = DiscIO::CreateWAD(path))
      return make(std::move(*wad));
  }

  if (extension == ".json")
  {
    if (const auto descriptor = DiscIO::ParseGameModDescriptorFile(path))
    {
      std::unique_ptr<BootParameters> result =
          GenerateFromFile(descriptor->base_file, std::move(boot_session_data_));
      if (!result)
      {
        PanicAlertFmtT("Could not recognize file {0}", descriptor->base_file);
        return nullptr;
      }

      // Riivolution patches only make sense on top of a disc.
      const bool patch_disc =
          descriptor->riivolution && std::holds_alternative<Disc>(result->parameters);
      if (patch_disc)
      {
        const DiscIO::VolumeDisc& volume = *std::get<Disc>(result->parameters).volume;
        auto patches = DiscIO::Riivolution::GenerateRiivolutionPatchesFromGameModDescriptor(
            *descriptor->riivolution, volume.GetGameID(), volume.GetRevision(),
            volume.GetDiscNumber());
        AddRiivolutionPatches(result.get(), std::move(patches));
      }
      return result;
    }
  }

  PanicAlertFmtT("Could not recognize file {0}", path);
  return {};
}

BootParameters::IPL::IPL(DiscIO::Region region_) : region(region_)
{
  path = Config::GetBootROMPath(Config::GetDirectoryForRegion(region));
}

BootParameters::IPL::IPL(DiscIO::Region region_, Disc&& disc_) : IPL(region_)
{
  disc = std::move(disc_);
}

bool CBoot::DVDRead(Core::System& system, const DiscIO::VolumeDisc& disc, u64 dvd_offset,
                    u32 output_address, u32 length, const DiscIO::Partition& partition)
{
  std::vector<u8> staging(length);
  const bool ok = disc.Read(dvd_offset, length, staging.data(), partition);
  if (ok)
    system.GetMemory().CopyToEmu(output_address, staging.data(), length);
  return ok;
}

bool CBoot::DVDReadDiscID(Core::System& system, const DiscIO::VolumeDisc& disc, u32 output_address)
{
  constexpr u32 DISC_HEADER_ID_SIZE = 0x20;
  std::array<u8, DISC_HEADER_ID_SIZE> disc_id;
  if (!disc.Read(0, disc_id.size(), disc_id.data(), DiscIO::PARTITION_NONE))
    return false;

  system.GetMemory().CopyToEmu(output_address, disc_id.data(), disc_id.size());

  // This is meant to be the drive's first read, so it leaves the
  // "disc ID not read" state now.
  system.GetDVDInterface().SetDriveState(DVD::DriveState::ReadyNoReadsMade);
  return true;
}

// Stand-in for the part of BS1 that matters when there's no full IPL boot:
// descramble the IPL ROM, put BS2 in RAM and jump to it. Hardware set-up
// done by BS1 is not reproduced.
bool CBoot::Load_BS2(Core::System& system, const std::string& boot_rom_filename)
{
  // CRC32 of known good IPL dumps (from Redump).
  enum : u32
  {
    IPL_NTSC_10 = 0x6DAC1F2A,      // DOL-001 USA/JPN, DOT-001, SL-GC10 (NTSC rev 1.0)
    IPL_NTSC_11 = 0xD5E6FEEA,      // DOL-001 USA/JPN, DOT-001 (NTSC rev 1.1)
    IPL_NTSC_12_001 = 0xD235E3F9,  // DOL-001 USA/JPN (NTSC rev 1.2)
    IPL_NTSC_12_101 = 0x86573808,  // DOL-101 USA/JPN (NTSC rev 1.2)
    IPL_MPAL_11 = 0x667D0B64,      // DOL-002 BRA (MPAL rev 1.1)
    IPL_PAL_10 = 0x4F319F43,       // DOL-001 EUR, DOT-001P (PAL rev 1.0)
    IPL_PAL_12 = 0xAD1B7F16,       // DOL-101 EUR (PAL rev 1.2)
    IPL_TRIFORCE = 0xD1883221,     // Triforce arcade (DEV rev 1.0)
  };

  // Official IPLs are 2 MiB; anything past that is ignored.
  constexpr u64 IPL_SIZE_LIMIT = 2 * 1024 * 1024;

  std::vector<u8> rom;
  {
    File::IOFile file(boot_rom_filename, "rb");
    if (!file)
      return false;
    rom.resize(static_cast<size_t>(std::min(file.GetSize(), IPL_SIZE_LIMIT)));
    if (!file.ReadArray(rom.data(), rom.size()))
      return false;
  }

  const u32 crc = Common::ComputeCRC32(rom.data(), rom.size());
  std::optional<bool> is_pal;  // empty for an unrecognised dump
  switch (crc)
  {
  case IPL_PAL_10:
  case IPL_PAL_12:
    is_pal = true;
    break;
  case IPL_NTSC_10:
  case IPL_NTSC_11:
  case IPL_NTSC_12_001:
  case IPL_NTSC_12_101:
  case IPL_MPAL_11:
  case IPL_TRIFORCE:
    is_pal = false;
    break;
  default:
    PanicAlertFmtT("The IPL file is not a known good dump. (CRC32: {0:x})", crc);
    break;
  }

  const DiscIO::Region region = SConfig::GetInstance().m_region;
  if (is_pal.has_value() && *is_pal != (region == DiscIO::Region::PAL))
  {
    PanicAlertFmtT("{0} IPL found in {1} directory. The disc might not be recognized",
                   *is_pal ? "PAL" : "NTSC", Config::GetDirectoryForRegion(region));
  }

  // Layout of the ROM: a 0x100 byte header, then the scrambled BS1/BS2
  // image. The first 0x700 bytes of it go to 0x81200000, the rest (from
  // ROM offset 0x820) to 0x81300000.
  constexpr size_t SCRAMBLED_START = 0x100;
  constexpr size_t SCRAMBLED_MAX = 0x1AFE00;
  constexpr size_t BS1_MAX = 0x700;
  constexpr size_t BS2_START = 0x820;

  if (rom.size() > SCRAMBLED_START)
  {
    const size_t span = std::min(rom.size() - SCRAMBLED_START, SCRAMBLED_MAX);
    ExpansionInterface::CEXIIPL::Descrambler(rom.data() + SCRAMBLED_START, static_cast<u32>(span));
  }

  // Real hardware starts executing at 0xFFF00000. Instead the boot code is
  // copied into RAM and the first few instructions are skipped (and their
  // effects reproduced below), entering at 0x81200150.
  auto& memory = system.GetMemory();
  if (rom.size() > SCRAMBLED_START)
  {
    memory.CopyToEmu(0x01200000, rom.data() + SCRAMBLED_START,
                     std::min(rom.size() - SCRAMBLED_START, BS1_MAX));
  }
  if (rom.size() > BS2_START)
  {
    memory.CopyToEmu(0x01300000, rom.data() + BS2_START,
                     std::min(rom.size() - BS2_START, SCRAMBLED_MAX));
  }

  // r3..r5 hold what the real BS1 hands over to the IPL.
  constexpr std::array<u32, 3> bs1_handoff{0xfff0001f, 0x00002030, 0x0000009c};
  PowerPC::PowerPCState& ppc_state = system.GetPPCState();
  for (size_t i = 0; i < bs1_handoff.size(); ++i)
    ppc_state.gpr[3 + i] = bs1_handoff[i];

  // Address translation and the FPU are both on.
  ppc_state.msr.IR = 1;
  ppc_state.msr.DR = 1;
  ppc_state.msr.FP = 1;

  ppc_state.spr[SPR_HID0] = 0x0011c464;
  // BAT3 maps the ROM area.
  ppc_state.spr[SPR_IBAT3U] = 0xfff0001f;
  ppc_state.spr[SPR_IBAT3L] = 0xfff00001;
  ppc_state.spr[SPR_DBAT3U] = 0xfff0001f;
  ppc_state.spr[SPR_DBAT3L] = 0xfff00001;
  SetupBAT(system, /*is_wii*/ false);

  if (crc == IPL_TRIFORCE)
  {
    // Logging entry points of the Triforce IPL and its apploader.
    HLE::Patch(system, 0x813048B8, "OSReport");
    HLE::Patch(system, 0x8130095C, "OSReport");
    AMMediaboard::FirmwareMap(true);
  }

  ppc_state.pc = 0x81200150;
  system.GetPowerPC().MSRUpdated();
  return true;
}

// Boots whichever kind of title the boot parameters hold.
class CBoot::TitleBooter
{
public:
  TitleBooter(Core::System& system, const Core::CPUThreadGuard& guard,
              const std::vector<DiscIO::Riivolution::Patch>& patches)
      : m_system(system), m_guard(guard), m_patches(patches)
  {
  }

  bool operator()(BootParameters::Disc& disc) const
  {
    NOTICE_LOG_FMT(BOOT, "Booting from disc: {}", disc.path);
    if (m_system.IsWii() && disc.volume && !HaveWiiKeys(*disc.volume))
      return false;
    const DiscIO::VolumeDisc* const volume =
        SetDisc(m_system.GetDVDInterface(), std::move(disc.volume), disc.auto_disc_change_paths);
    AchievementManager::GetInstance().LoadGame(volume);

    if (volume == nullptr)
      return false;
    if (!EmulatedBS2(m_system, m_guard, m_system.IsWii(), *volume, m_patches))
      return false;

    SConfig::OnTitleDirectlyBooted(m_guard);
    return true;
  }

  bool operator()(const BootParameters::Executable& executable) const
  {
    NOTICE_LOG_FMT(BOOT, "Booting from executable: {}", executable.path);

    BootExecutableReader& reader = *executable.reader;
    if (!reader.IsValid())
      return false;

    SetDefaultDisc(m_system.GetDVDInterface());
    PrepareCPUForExecutable();

    if (m_system.IsWii())
      PrepareWiiForExecutable();
    else
      SetupGCMemory(m_system, m_guard);

    if (!reader.LoadIntoMemory(m_system))
    {
      PanicAlertFmtT("Failed to load the executable to memory.");
      return false;
    }

    FinishTitleBoot(m_guard, nullptr);
    m_system.GetPPCState().pc = reader.GetEntryPoint();

    // Replace whatever symbols an earlier session left behind with the
    // executable's own, and hook them.
    PPCSymbolDB& symbols = m_system.GetPPCSymbolDB();
    bool symbols_changed = symbols.Clear();
    if (reader.LoadSymbols(m_guard, symbols, PathToFileName(executable.path)))
    {
      symbols_changed = true;
      HLE::PatchFunctions(m_system);
    }
    if (symbols_changed)
      Host_PPCSymbolsChanged();

    return true;
  }

  bool operator()(const DiscIO::VolumeWAD& wad) const
  {
    if (!HaveWiiKeys(wad))
      return false;
    SetDefaultDisc(m_system.GetDVDInterface());
    if (!Boot_WiiWAD(m_system, wad))
      return false;
    FinishTitleBoot(m_guard, &wad);
    return true;
  }

  bool operator()(const BootParameters::NANDTitle& nand_title) const
  {
    // The title's ticket is only readable once IOS runs; any keys file will do here, and a
    // ticket for the other kind of console then fails in ES like on hardware.
    if (!IOS::HLE::IOSC::HasConsoleKeys())
    {
      IOS::HLE::IOSC::ReportMissingKeys(IOS::HLE::IOSC::ConsoleType::Retail);
      return false;
    }
    SetDefaultDisc(m_system.GetDVDInterface());
    if (!BootNANDTitle(m_system, nand_title.id))
      return false;
    FinishTitleBoot(m_guard, nullptr);
    return true;
  }

  bool operator()(const BootParameters::IPL& ipl) const
  {
    NOTICE_LOG_FMT(BOOT, "Booting GC IPL: {}", ipl.path);
    if (!File::Exists(ipl.path))
    {
      if (ipl.disc.has_value())
        PanicAlertFmtT("Cannot start the game, because the GC IPL could not be found.");
      else
        PanicAlertFmtT("Cannot find the GC IPL.");
      return false;
    }

    if (!Load_BS2(m_system, ipl.path))
      return false;

    const DiscIO::VolumeDisc* volume = nullptr;
    if (ipl.disc.has_value())
    {
      NOTICE_LOG_FMT(BOOT, "Inserting disc: {}", ipl.disc->path);
      volume = SetDisc(m_system.GetDVDInterface(), DiscIO::CreateDiscForCore(ipl.disc->path),
                       ipl.disc->auto_disc_change_paths);
    }

    FinishTitleBoot(m_guard, volume);
    return true;
  }

  bool operator()(const BootParameters::DFF& dff) const
  {
    NOTICE_LOG_FMT(BOOT, "Booting DFF: {}", dff.dff_path);
    AchievementManager::GetInstance().LoadGame(nullptr);
    return m_system.GetFifoPlayer().Open(dff.dff_path);
  }

private:
  // Machine state the IPL would otherwise have left behind.
  void PrepareCPUForExecutable() const
  {
    const bool wii = m_system.IsWii();
    SetupMSR(m_system);
    SetupHID(m_system.GetPPCState(), wii);
    SetupBAT(m_system, wii);
    CopyDefaultExceptionHandlers(m_system);
  }

  void PrepareWiiForExecutable() const
  {
    // Any valid address will do for the stack; this one is what homebrew
    // binaries are commonly started with.
    m_system.GetPPCState().gpr[1] = 0x8004d4bc;

    // Without a TMD there's no requested IOS. IOS58 is what the Homebrew
    // Channel uses.
    SetupWiiMemory(m_system, IOS::HLE::IOSC::ConsoleType::Retail);
    m_system.GetIOS()->BootIOS(Titles::IOS(58));

    // Apploaders mirror the IOS version into the apploader version word,
    // and older OSInit versions look at it.
    constexpr u32 IOS_VERSION_WORD = 0x3140;
    constexpr u32 APPLOADER_VERSION_WORD = 0x3188;
    auto& memory = m_system.GetMemory();
    memory.Write_U32(memory.Read_U32(IOS_VERSION_WORD), APPLOADER_VERSION_WORD);
  }

  Core::System& m_system;
  const Core::CPUThreadGuard& m_guard;
  const std::vector<DiscIO::Riivolution::Patch>& m_patches;
};

// Entry point of title booting, after the core and boot manager have run.
bool CBoot::BootUp(Core::System& system, const Core::CPUThreadGuard& guard,
                   std::unique_ptr<BootParameters> boot)
{
  SConfig& config = SConfig::GetInstance();

  // Triforce hardware is region free but always runs in NTSC video mode.
  if (system.IsTriforce())
    config.m_region = DiscIO::Region::NTSC_J;

  // NTSC timings also apply to PAL Wii consoles running in PAL60.
  const bool ntsc_timing =
      DiscIO::IsNTSC(config.m_region) || (system.IsWii() && Config::Get(Config::SYSCONF_PAL60));
  system.GetVideoInterface().Preset(ntsc_timing);

  const TitleBooter booter(system, guard, boot->riivolution_patches);
  if (!std::visit(booter, boot->parameters))
    return false;

  DiscIO::Riivolution::ApplyGeneralMemoryPatches(guard, boot->riivolution_patches);
  return true;
}

// BootExecutableReader --------------------------------------------------------------------------

BootExecutableReader::~BootExecutableReader() = default;

BootExecutableReader::BootExecutableReader(std::vector<u8> bytes) : m_bytes(std::move(bytes))
{
}

// Reads the whole file from its start; a file that can't be opened gives no bytes.
BootExecutableReader::BootExecutableReader(File::IOFile file)
{
  file.Seek(0, File::SeekOrigin::Begin);
  m_bytes.resize(file.GetSize());
  file.ReadBytes(m_bytes.data(), m_bytes.size());
}

BootExecutableReader::BootExecutableReader(const std::string& file_name)
    : BootExecutableReader(File::IOFile{file_name, "rb"})
{
}

// The checksum is the 32-bit sum of every word after the checksum itself.
void StateFlags::UpdateChecksum()
{
  constexpr size_t word_count = (sizeof(StateFlags) - sizeof(checksum)) / sizeof(u32);
  std::array<u32, word_count> words;
  std::memcpy(words.data(), &flags, sizeof(words));

  u32 sum = 0;
  for (const u32 word : words)
    sum += word;
  checksum = sum;
}

void UpdateStateFlags(const std::function<void(StateFlags*)>& update_function)
{
  CreateSystemMenuTitleDirs();

  const std::string state_path = Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_STATE;
  const auto fs = Core::System::GetInstance().GetIOS()->GetFS();
  constexpr IOS::HLE::FS::Mode rw = IOS::HLE::FS::Mode::ReadWrite;
  const auto file =
      fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, state_path, {rw, rw, rw});
  if (!file)
    return;

  // An existing file of the wrong size is treated as empty.
  const bool has_valid_size = file->GetStatus()->size == sizeof(StateFlags);
  StateFlags state{};
  if (has_valid_size)
    (void)file->Read(&state, 1);

  update_function(&state);
  state.UpdateChecksum();

  // Overwrite the file in place.
  constexpr IOS::HLE::FS::SeekMode from_start = IOS::HLE::FS::SeekMode::Set;
  (void)file->Seek(0, from_start);
  (void)file->Write(&state, 1);
}

void CreateSystemMenuTitleDirs()
{
  const auto& es_core = Core::System::GetInstance().GetIOS()->GetESCore();
  es_core.CreateTitleDirectories(Titles::SYSTEM_MENU, IOS::SYSMENU_GID);
}

void AddRiivolutionPatches(BootParameters* boot_params,
                           std::vector<DiscIO::Riivolution::Patch> riivolution_patches)
{
  if (riivolution_patches.empty())
    return;

  auto* const disc = std::get_if<BootParameters::Disc>(&boot_params->parameters);
  if (disc == nullptr)
    return;

  // Wrap the disc in a virtual one whose files are patched on the fly.
  auto patch_sys_files = [&](std::vector<DiscIO::FSTBuilderNode>* fst) {
    DiscIO::Riivolution::ApplyPatchesToFiles(
        riivolution_patches, DiscIO::Riivolution::PatchIndex::DolphinSysFiles, fst, nullptr);
  };
  auto patch_file_system = [&](std::vector<DiscIO::FSTBuilderNode>* fst,
                               DiscIO::FSTBuilderNode* dol_node) {
    DiscIO::Riivolution::ApplyPatchesToFiles(
        riivolution_patches, DiscIO::Riivolution::PatchIndex::FileSystem, fst, dol_node);
  };
  disc->volume = DiscIO::CreateDisc(DiscIO::DirectoryBlobReader::Create(
      std::move(disc->volume), patch_sys_files, patch_file_system));
  boot_params->riivolution_patches = std::move(riivolution_patches);
}
