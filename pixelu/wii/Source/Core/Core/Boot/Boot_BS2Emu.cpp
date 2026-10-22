// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// High-level emulation of the second stage bootloader (BS2): instead of
// running the IPL or the system menu, set up the machine the way they would
// leave it and run the disc's apploader directly.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/SettingsHandler.h"

#include "Core/Boot/Boot.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/DI/DI.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"
#include "DiscIO/RiivolutionPatcher.h"
#include "DiscIO/VolumeDisc.h"

#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

namespace
{
constexpr u32 RFI = 0x4c000064;
constexpr u32 BLR = 0x4E800020;

// Where the apploader is told to put its three function pointers.
constexpr u32 APPLOADER_FUNCTIONS_GC = 0x80003100;
constexpr u32 APPLOADER_FUNCTIONS_WII = 0x80004000;

// Scratch area handed to the apploader: a report callback stub, followed by
// the three output words of iAppLoaderMain (destination, length, offset).
constexpr u32 APPLOADER_SCRATCH = 0x81300000;
constexpr u32 APPLOADER_OUT_DEST = APPLOADER_SCRATCH + 4;
constexpr u32 APPLOADER_OUT_LENGTH = APPLOADER_SCRATCH + 8;
constexpr u32 APPLOADER_OUT_OFFSET = APPLOADER_SCRATCH + 0xc;

// The time base runs at a quarter of the bus clock.
constexpr u64 TIMEBASE_TICKS_PER_SECOND = 40500000ULL;

// Seeds the OS's notion of the current time, which normally comes from the
// IPL reading the RTC.
void PresetTimeBaseTicks(Core::System& system, const Core::CPUThreadGuard& guard)
{
  const u64 seconds =
      ExpansionInterface::CEXIIPL::GetEmulatedTime(system, ExpansionInterface::CEXIIPL::GC_EPOCH);
  PowerPC::MMU::HostWrite<u64>(guard, seconds * TIMEBASE_TICKS_PER_SECOND, 0x800030D8);
}

void PointAppLoaderMainOutputs(PowerPC::PowerPCState& ppc_state)
{
  ppc_state.gpr[3] = APPLOADER_OUT_DEST;
  ppc_state.gpr[4] = APPLOADER_OUT_LENGTH;
  ppc_state.gpr[5] = APPLOADER_OUT_OFFSET;
}

// Region implied by the second character of a setting.txt CODE value.
DiscIO::Region CodeRegion(char c)
{
  switch (c)
  {
  case 'J':  // Japan
  case 'T':  // Taiwan
    return DiscIO::Region::NTSC_J;
  case 'B':  // Brazil
  case 'M':  // Middle East
  case 'R':  // Argentina
  case 'S':  // unknown, treated as Americas
  case 'U':  // USA
  case 'W':  // unknown, treated as Americas
    return DiscIO::Region::NTSC_U;
  case 'A':  // Australia
  case 'E':  // Europe
    return DiscIO::Region::PAL;
  case 'K':  // Korea
    return DiscIO::Region::NTSC_K;
  default:
    return DiscIO::Region::Unknown;
  }
}

// The system menu keeps a play record on the NAND; start with a blank one.
void WriteEmptyPlayRecord()
{
  CreateSystemMenuTitleDirs();
  const std::string path = Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/play_rec.dat";
  const auto fs = Core::System::GetInstance().GetIOS()->GetFS();
  constexpr IOS::HLE::FS::Mode rw = IOS::HLE::FS::Mode::ReadWrite;
  const auto file = fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, path, {rw, rw, rw});
  if (!file)
    return;
  const std::vector<u8> blank(0x80);
  (void)file->Write(blank.data(), blank.size());
}
}  // namespace

// Calls the guest function at `address` and steps the CPU until it returns.
// The return lands on address zero, which is how completion is detected.
void CBoot::RunFunction(Core::System& system, u32 address)
{
  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();

  LR(ppc_state) = 0;
  ppc_state.pc = address;
  while (ppc_state.pc != 0)
    power_pc.SingleStep();
}

// Translation and floating point on, exceptions recoverable.
void CBoot::SetupMSR(Core::System& system)
{
  auto& msr = system.GetPPCState().msr;
  msr.FP = 1;
  msr.IR = 1;
  msr.DR = 1;
  msr.RI = 1;
  system.GetPowerPC().MSRUpdated();
}

// Cache and paired-single configuration as left by the IPL. The results are
// HID0 = 0x0011c464 (GC) / 0x0011c664 (Wii), HID2 = 0xe0000000 and
// HID4 = 0 (GC) / 0x83900000 (Wii). HID1 keeps its reset value.
void CBoot::SetupHID(PowerPC::PowerPCState& ppc_state, bool is_wii)
{
  auto& hid0 = HID0(ppc_state);
  hid0.NHR = 1;
  hid0.ICE = 1;  // Datel discs refuse to boot with the icache disabled.
  hid0.DCE = 1;
  hid0.DCFI = 1;
  hid0.DCFA = 1;
  hid0.BTIC = 1;
  hid0.BHT = 1;
  hid0.DPM = 1;
  if (is_wii)
    hid0.SPD = 1;

  auto& hid2 = HID2(ppc_state);
  hid2.LSQE = 1;
  hid2.WPE = 1;
  hid2.PSE = 1;

  if (!is_wii)
    return;

  auto& hid4 = HID4(ppc_state);
  hid4.reserved_3 = 1;
  hid4.SBE = 1;
  hid4.ST0 = 1;
  hid4.LPE = 1;
  hid4.L2CFI = 1;
}

// The standard OS mappings: cached and uncached MEM1, plus MEM2 on Wii.
void CBoot::SetupBAT(Core::System& system, bool is_wii)
{
  struct BatPair
  {
    int upper;
    int lower;
    u32 upper_value;
    u32 lower_value;
  };
  static constexpr std::array<BatPair, 3> common_bats = {{
      {SPR_IBAT0U, SPR_IBAT0L, 0x80001fff, 0x00000002},
      {SPR_DBAT0U, SPR_DBAT0L, 0x80001fff, 0x00000002},
      {SPR_DBAT1U, SPR_DBAT1L, 0xc0001fff, 0x0000002a},
  }};
  static constexpr std::array<BatPair, 3> wii_bats = {{
      {SPR_IBAT4U, SPR_IBAT4L, 0x90001fff, 0x10000002},
      {SPR_DBAT4U, SPR_DBAT4L, 0x90001fff, 0x10000002},
      {SPR_DBAT5U, SPR_DBAT5L, 0xd0001fff, 0x1000002a},
  }};

  auto& ppc_state = system.GetPPCState();
  const auto apply = [&ppc_state](const auto& table) {
    for (const BatPair& bat : table)
    {
      ppc_state.spr[bat.upper] = bat.upper_value;
      ppc_state.spr[bat.lower] = bat.lower_value;
    }
  };

  apply(common_bats);
  if (is_wii)
  {
    apply(wii_bats);
    // BATs 4-7 only exist with this bit set.
    HID4(ppc_state).SBE = 1;
  }

  auto& mmu = system.GetMMU();
  mmu.DBATUpdated();
  mmu.IBATUpdated();
}

// Runs the disc's apploader through its three entry points. The apploader
// asks for chunks of the disc (main.dol, the FST, ...) one at a time; each
// request is served by copying from the disc. See YAGCD chapter 13.
bool CBoot::RunApploader(Core::System& system, const Core::CPUThreadGuard& guard, bool is_wii,
                         const DiscIO::VolumeDisc& volume,
                         const std::vector<DiscIO::Riivolution::Patch>& riivolution_patches)
{
  // The apploader header sits at a fixed disc offset, its code right after.
  constexpr u32 APPLOADER_HEADER = 0x2440;
  constexpr u32 APPLOADER_CODE = APPLOADER_HEADER + 0x20;
  constexpr u32 APPLOADER_LOAD_ADDRESS = 0x01200000;

  const DiscIO::Partition partition = volume.GetGamePartition();
  const std::optional<u32> entry = volume.ReadSwapped<u32>(APPLOADER_HEADER + 0x10, partition);
  const std::optional<u32> size = volume.ReadSwapped<u32>(APPLOADER_HEADER + 0x14, partition);
  const std::optional<u32> trailer = volume.ReadSwapped<u32>(APPLOADER_HEADER + 0x18, partition);
  const bool header_ok =
      entry && size && trailer && *entry != UINT32_MAX && *size + *trailer != UINT32_MAX;
  if (!header_ok)
  {
    INFO_LOG_FMT(BOOT, "Invalid apploader. Your disc image is probably corrupted.");
    return false;
  }
  DVDRead(system, volume, APPLOADER_CODE, APPLOADER_LOAD_ADDRESS, *size + *trailer, partition);

  auto& ppc_state = system.GetPPCState();
  auto& mmu = system.GetMMU();
  auto& branch_watch = system.GetPowerPC().GetBranchWatch();

  // Optionally keep the apploader's branches out of the branch watch.
  const bool was_recording = branch_watch.GetRecordingActive();
  if (system.IsBranchWatchIgnoreApploader())
    branch_watch.SetRecordingActive(guard, false);

  // Entry point: fills in the addresses of init, main and close.
  DEBUG_LOG_FMT(BOOT, "Call iAppLoaderEntry");
  const u32 table = is_wii ? APPLOADER_FUNCTIONS_WII : APPLOADER_FUNCTIONS_GC;
  for (u32 slot = 0; slot < 3; ++slot)
    ppc_state.gpr[3 + slot] = table + 4 * slot;
  RunFunction(system, *entry);
  const u32 init_function = mmu.Read<u32>(table);
  const u32 main_function = mmu.Read<u32>(table + 4);
  const u32 close_function = mmu.Read<u32>(table + 8);

  // Init gets a report callback; route it to the HLE logger by planting a
  // bare `blr` and hooking it.
  DEBUG_LOG_FMT(BOOT, "Call iAppLoaderInit");
  PowerPC::MMU::HostWrite<u32>(guard, BLR, APPLOADER_SCRATCH);
  HLE::Patch(system, APPLOADER_SCRATCH, "AppLoaderReport");
  ppc_state.gpr[3] = APPLOADER_SCRATCH;
  RunFunction(system, init_function);

  // Main returns non-zero for as long as it wants more data. Some
  // unlicensed discs drive this from their own table rather than from the
  // DOL header, so no assumptions are made about the requests.
  DEBUG_LOG_FMT(BOOT, "Call iAppLoaderMain");
  // Wii discs give offsets in units of 4 bytes.
  const u32 offset_shift = is_wii ? 2 : 0;
  for (;;)
  {
    PointAppLoaderMainOutputs(ppc_state);
    RunFunction(system, main_function);
    if (ppc_state.gpr[3] == 0)
      break;

    const u32 dest = mmu.Read<u32>(APPLOADER_OUT_DEST);
    const u32 length = mmu.Read<u32>(APPLOADER_OUT_LENGTH);
    const u32 disc_offset = mmu.Read<u32>(APPLOADER_OUT_OFFSET) << offset_shift;
    INFO_LOG_FMT(BOOT, "DVDRead: offset: {:08x}   memOffset: {:08x}   length: {}", disc_offset,
                 dest, length);
    DVDRead(system, volume, disc_offset, dest, length, partition);
    DiscIO::Riivolution::ApplyApploaderMemoryPatches(guard, riivolution_patches, dest, length);
  }

  // Close hands back the game's entry point.
  DEBUG_LOG_FMT(BOOT, "call iAppLoaderClose");
  RunFunction(system, close_function);
  HLE::UnPatch(system, "AppLoaderReport");
  ppc_state.pc = ppc_state.gpr[3];

  branch_watch.SetRecordingActive(guard, was_recording);

  if (system.IsTriforce())
  {
    AMMediaboard::InitDIMM(volume);

    // The Triforce obfuscates its disc registers with keys derived from the
    // first words of the game ID.
    auto& memory = system.GetMemory();
    const u32 key0 = memory.Read_U32(0);
    const u32 key1 = memory.Read_U32(4);
    const u32 key2 = memory.Read_U32(8);
    AMMediaboard::InitKeys(key0, key1, key2);
    AMMediaboard::FirmwareMap(false);
  }

  return true;
}

// The low-memory globals the GameCube IPL leaves for the OS (YAGCD 4.2.1).
void CBoot::SetupGCMemory(Core::System& system, const Core::CPUThreadGuard& guard)
{
  // A devkit console ID is reported rather than retail (3): some games take
  // different EXI paths with the retail ID and fail (Ikaruga, for one).
  const u32 console_type = static_cast<u32>(Core::ConsoleType::LatestDevkit);
  const u32 vi_mode = DiscIO::IsNTSC(SConfig::GetInstance().m_region) ? 0 : 1;

  struct Global
  {
    u32 address;
    u32 value;
  };
  const std::array<Global, 10> globals = {{
      {0x80000020, 0x0D15EA5E},                           // boot magic: booted from the boot ROM
      {0x80000028, system.GetMemory().GetRamSizeReal()},  // physical memory size
      {0x8000002C, console_type},
      {0x800000CC, vi_mode},     // video mode, normally set by the IPL's VI init
      {0x800000d0, 0x01000000},  // ARAM size (16 MiB internal, no expansion)
      {0x800000F8, 0x09a7ec80},  // bus clock
      {0x800000FC, 0x1cf7c580},  // CPU clock
      {0x80000300, RFI},         // DSI handler
      {0x80000800, RFI},         // FPU unavailable handler
      {0x80000C00, RFI},         // system call handler
  }};
  for (const Global& global : globals)
    PowerPC::MMU::HostWrite<u32>(guard, global.value, global.address);

  PresetTimeBaseTicks(system, guard);
}

// GameCube BS2: prepare the machine, then let the apploader (loaded at
// 0x81200000) pull the game in.
bool CBoot::EmulatedBS2_GC(Core::System& system, const Core::CPUThreadGuard& guard,
                           const DiscIO::VolumeDisc& volume,
                           const std::vector<DiscIO::Riivolution::Patch>& riivolution_patches)
{
  INFO_LOG_FMT(BOOT, "Faking GC BS2...");

  auto& ppc_state = system.GetPPCState();
  SetupMSR(system);
  SetupHID(ppc_state, /*is_wii*/ false);
  SetupBAT(system, /*is_wii*/ false);
  SetupGCMemory(system, guard);

  // The IPL sets the last post-transform matrix to identity. Some Datel
  // discs never load it but still enable dual texture transforms, which
  // would otherwise sample every texture at (0, 0).
  constexpr u32 IDENTITY_ROW = 0x3d;
  for (u32 row = 0; row < 3; ++row)
    xfmem.postMatrices[(IDENTITY_ROW + row) * 4 + row] = 1.0f;
  g_vertex_manager->Flush();
  system.GetXFStateManager().InvalidateXFRange(XFMEM_POSTMATRICES + IDENTITY_ROW * 4,
                                               XFMEM_POSTMATRICES_END);

  DVDReadDiscID(system, volume, 0x00000000);

  // Audio streaming settings come from the disc header.
  auto& memory = system.GetMemory();
  auto& dvd = system.GetDVDInterface();
  if (memory.Read_U8(0x80000008) == 0)
  {
    dvd.AudioBufferConfig(false, 0);
  }
  else
  {
    u8 buffer_size = memory.Read_U8(0x80000009);
    // BS2 treats a size of zero as its default of 10.
    if (buffer_size == 0)
      buffer_size = 10;
    dvd.AudioBufferConfig(true, buffer_size);
  }

  // Register values BS2 leaves behind: stack pointer and the two small
  // data area bases (the latter is used by Luigi's Mansion's apploader).
  const bool ntsc = DiscIO::IsNTSC(SConfig::GetInstance().m_region);
  ppc_state.gpr[1] = ntsc ? 0x81566550 : 0x815edca8;
  ppc_state.gpr[2] = ntsc ? 0x81465cc0 : 0x814b5b20;
  ppc_state.gpr[13] = ntsc ? 0x81465320 : 0x814b4fc0;

  return RunApploader(system, guard, /*is_wii*/ false, volume, riivolution_patches);
}

// Writes setting.txt to the NAND and memory and fills in the low-memory
// globals that the system menu and IOS would normally leave for a game.
bool CBoot::SetupWiiMemory(Core::System& system, IOS::HLE::IOSC::ConsoleType console_type)
{
  static const std::map<DiscIO::Region, const RegionSetting> defaults_by_region = {
      {DiscIO::Region::NTSC_J, {"JPN", "NTSC", "JP", "LJH"}},
      {DiscIO::Region::NTSC_U, {"USA", "NTSC", "US", "LU"}},
      {DiscIO::Region::PAL, {"EUR", "PAL", "EU", "LEH"}},
      {DiscIO::Region::NTSC_K, {"KOR", "NTSC", "KR", "LKH"}}};
  const DiscIO::Region region = SConfig::GetInstance().m_region;
  RegionSetting setting = defaults_by_region.find(region)->second;

  std::string serial;
  std::string model = "RVL-001(" + setting.area + ")";

  CreateSystemMenuTitleDirs();
  const std::string settings_path = Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING;
  const auto fs = system.GetIOS()->GetFS();

  // Carry over the serial number and model from an existing setting.txt.
  // Its region fields are kept only if they match the title's region.
  {
    Common::SettingsBuffer existing;
    const auto old_file =
        fs->OpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, settings_path, IOS::HLE::FS::Mode::Read);
    if (old_file && old_file->Read(existing.data(), existing.size()))
    {
      const Common::SettingsReader reader(existing);
      serial = reader.GetValue("SERNO");
      model = reader.GetValue("MODEL");

      bool keep_region = Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS);
      if (!keep_region)
      {
        const std::string code = reader.GetValue("CODE");
        keep_region = code.size() >= 2 && CodeRegion(code[1]) == region;
      }

      if (keep_region)
      {
        setting.area = reader.GetValue("AREA");
        setting.video = reader.GetValue("VIDEO");
        setting.game = reader.GetValue("GAME");
        setting.code = reader.GetValue("CODE");
      }
      else if (const size_t open = model.find('('); open != std::string::npos)
      {
        // Keep the model number but correct the region suffix.
        model = model.substr(0, open) + '(' + setting.area + ')';
      }
    }
  }
  fs->Delete(IOS::SYSMENU_UID, IOS::SYSMENU_GID, settings_path);

  if (!serial.empty() && serial != "000000000")
  {
    INFO_LOG_FMT(BOOT, "Using serial number: {}", serial);
  }
  else
  {
    serial =
        Core::WantsDeterminism() ? "123456789" : Common::SettingsWriter::GenerateSerialNumber();
    INFO_LOG_FMT(BOOT, "No previous serial number found, generated one instead: {}", serial);
  }

  Common::SettingsWriter writer;
  writer.AddSetting("AREA", setting.area);
  writer.AddSetting("MODEL", model);
  writer.AddSetting("DVD", "0");
  writer.AddSetting("MPCH", "0x7FFE");
  writer.AddSetting("CODE", setting.code);
  writer.AddSetting("SERNO", serial);
  writer.AddSetting("VIDEO", setting.video);
  writer.AddSetting("GAME", setting.game);
  const auto& encoded = writer.GetBytes();

  constexpr IOS::HLE::FS::Mode rw = IOS::HLE::FS::Mode::ReadWrite;
  const auto new_file =
      fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, settings_path, {rw, rw, rw});
  if (!new_file || !new_file->Write(encoded.data(), encoded.size()))
  {
    PanicAlertFmtT("SetupWiiMemory: Can't create setting.txt file");
    return false;
  }

  auto& memory = system.GetMemory();

  // setting.txt (256 bytes) also lives in low memory.
  memory.CopyToEmu(0x3800, encoded.data(), encoded.size());

  INFO_LOG_FMT(BOOT, "Setup Wii Memory...");

  // Fixed globals, mostly as documented on WiiBrew. The FST location and
  // size (0x38/0x3c) and the copyright code (0x60) are filled in later by
  // the apploader and the game.
  const u32 mem1_size = memory.GetRamSizeReal();
  const u32 board = static_cast<u32>(console_type == IOS::HLE::IOSC::ConsoleType::RVT ?
                                         Core::ConsoleType::NDEV2_1 :
                                         Core::ConsoleType::RVL_Retail3);
  const u32 vi_mode = DiscIO::IsNTSC(SConfig::GetInstance().m_region) ? 0 : 1;

  struct Global
  {
    u32 address;
    u32 value;
    u8 width;
  };
  // Order matters: 0x30e6 is written twice and the later value wins.
  const std::array<Global, 23> globals = {{
      {0x00000020, 0x0D15EA5E, 4},  // boot magic
      {0x00000024, 0x00000001, 4},
      {0x00000028, mem1_size, 4},   // MEM1 size
      {0x0000002c, board, 4},       // board model
      {0x00000030, 0x00000000, 4},  // arena low
      {0x00000034, 0x817FEC60, 4},  // arena high
      {0x000000e4, 0x8008f7b8, 4},  // thread init
      {0x000000f0, mem1_size, 4},   // simulated memory size
      {0x000000f4, 0x8179b500, 4},  // __start
      {0x000000f8, 0x0e7be2c0, 4},  // bus clock
      {0x000000fc, 0x2B73A840, 4},  // CPU clock
      {0x000030e6, 0x0000, 2},      // console type
      {0x000030c0, 0x00000000, 4},  // EXI
      {0x000030c4, 0x00000000, 4},  // EXI
      {0x000030dc, 0x00000000, 4},  // time
      {0x000030d8, 0xffffffff, 4},  // set by every official NAND title
      {0x000030e6, 0x8201, 2},      // dev console, debug capable
      {0x000030f0, 0x00000000, 4},  // apploader
      // "Devkit boot program version", written by the system menu. 0x0113
      // (v1.13, the newest) is accepted by every apploader.
      {0x0000315e, 0x0113, 2},
      {0x0000315c, 0x80, 1},        // OSInit
      {0x000030e0, 0x0000, 2},      // PADInit
      {0x00003184, 0x80000000, 4},  // game ID address
      {0x000000CC, vi_mode, 4},     // video mode, as the IPL's VI init leaves it
  }};
  for (const Global& global : globals)
  {
    switch (global.width)
    {
    case 1:
      memory.Write_U8(static_cast<u8>(global.value), global.address);
      break;
    case 2:
      memory.Write_U16(static_cast<u16>(global.value), global.address);
      break;
    default:
      memory.Write_U32(global.value, global.address);
      break;
    }
  }

  // Clear the OS globals area at 0x3000-0x303b.
  for (u32 address = 0x3000; address <= 0x3038; address += 4)
    memory.Write_U32(0, address);

  return true;
}

// Wii BS2: do what the system menu does when launching a disc, then let the
// apploader pull the game in.
bool CBoot::EmulatedBS2_Wii(Core::System& system, const Core::CPUThreadGuard& guard,
                            const DiscIO::VolumeDisc& volume,
                            const std::vector<DiscIO::Riivolution::Patch>& riivolution_patches)
{
  INFO_LOG_FMT(BOOT, "Faking Wii BS2...");
  if (volume.GetVolumeType() != DiscIO::Platform::WiiDisc)
    return false;

  const DiscIO::Partition partition = volume.GetGamePartition();
  const IOS::ES::TMDReader tmd = volume.GetTMD(partition);
  if (!tmd.IsValid())
    return false;

  WriteEmptyPlayRecord();
  UpdateStateFlags([](StateFlags* state) {
    state->flags = 0xc1;
    state->type = 0xff;
    state->discstate = 0x01;
  });

  // The RTC flags are deliberately left alone. The system menu clears them,
  // but it also refreshes its disc cache at the same time; clearing them
  // without that makes the disc channel show stale data.

  // Before launching a disc, the system menu stores the type and offset of
  // the data partition entry (from the partition table at 0x40020).
  auto& memory = system.GetMemory();
  const DiscIO::Partition data_partition = volume.GetGamePartition();
  memory.Write_U32(0, 0x3194);
  memory.Write_U32(static_cast<u32>(data_partition.offset >> 2), 0x3198);

  const s32 forced_ios = Config::Get(Config::MAIN_OVERRIDE_BOOT_IOS);
  const u64 ios = forced_ios < 0 ? tmd.GetIOSId() : Titles::IOS(static_cast<u32>(forced_ios));

  const auto console_type = volume.GetTicket(data_partition).GetConsoleType();
  if (!SetupWiiMemory(system, console_type))
    return false;
  if (!system.GetIOS()->BootIOS(ios))
    return false;

  const auto di =
      std::static_pointer_cast<IOS::HLE::DIDevice>(system.GetIOS()->GetDeviceByName("/dev/di"));
  di->InitializeIfFirstTime();
  di->ChangePartition(data_partition);

  DVDReadDiscID(system, volume, 0x00000000);

  // The 4-byte game ID at 0x3180 stays put for the whole session, unlike the
  // copy at 0, and games compare the two while booting.
  DVDRead(system, volume, 0, 0x3180, 4, partition);

  auto& ppc_state = system.GetPPCState();
  SetupMSR(system);
  SetupHID(ppc_state, /*is_wii*/ true);
  SetupBAT(system, /*is_wii*/ true);

  for (const u32 handler : {0x00000300u, 0x00000800u, 0x00000C00u})
    memory.Write_U32(RFI, handler);

  ppc_state.gpr[1] = 0x816ffff0;  // stack pointer

  if (!RunApploader(system, guard, /*is_wii*/ true, volume, riivolution_patches))
    return false;

  // The apploader may have overwritten what the RAM override relies on.
  IOS::HLE::RAMOverrideForIOSMemoryValues(memory, IOS::HLE::MemorySetupType::IOSReload);

  // Note: this records the running title from `volume`, which must be the
  // disc that is actually in the emulated drive.
  system.GetIOS()->GetESDevice()->DIVerify(tmd, volume.GetTicket(partition));

  return true;
}

// For Wii, the disc behind `volume` must be the one in the emulated drive.
bool CBoot::EmulatedBS2(Core::System& system, const Core::CPUThreadGuard& guard, bool is_wii,
                        const DiscIO::VolumeDisc& volume,
                        const std::vector<DiscIO::Riivolution::Patch>& riivolution_patches)
{
  if (is_wii)
    return EmulatedBS2_Wii(system, guard, volume, riivolution_patches);
  return EmulatedBS2_GC(system, guard, volume, riivolution_patches);
}
