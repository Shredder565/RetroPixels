// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/IOS.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"

#include "Core/Boot/AncastTypes.h"
#include "Core/Boot/DolReader.h"
#include "Core/Boot/ElfReader.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/WII_IPC.h"
#include "Core/IOS/Crypto/AesDevice.h"
#include "Core/IOS/Crypto/Sha.h"
#include "Core/IOS/DI/DI.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/DeviceStub.h"
#include "Core/IOS/DolphinDevice.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/FileSystemProxy.h"
#include "Core/IOS/MIOS.h"
#include "Core/IOS/Network/IP/Top.h"
#include "Core/IOS/Network/KD/NetKDRequest.h"
#include "Core/IOS/Network/KD/NetKDTime.h"
#include "Core/IOS/Network/NCD/Manage.h"
#include "Core/IOS/Network/SSL.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/IOS/Network/WD/Command.h"
#include "Core/IOS/SDIO/SDIOSlot0.h"
#include "Core/IOS/STM/STM.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/BTReal.h"
#include "Core/IOS/USB/OH0/OH0.h"
#include "Core/IOS/USB/OH0/OH0Device.h"
#include "Core/IOS/USB/USB_HID/HIDv4.h"
#include "Core/IOS/USB/USB_HID/HIDv5.h"
#include "Core/IOS/USB/USB_KBD.h"
#include "Core/IOS/USB/USB_VEN/VEN.h"
#include "Core/IOS/VersionInfo.h"
#include "Core/IOS/WFS/WFSI.h"
#include "Core/IOS/WFS/WFSSRV.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

namespace IOS::HLE
{
namespace
{
// The IPC event carries either a request address (with this bit set, meaning "a new request
// has arrived") or a reply address (bit clear, meaning "this answer is ready for the PPC").
constexpr u64 ENQUEUE_REQUEST_FLAG = 0x100000000ULL;

CoreTiming::EventType* s_ipc_event;
CoreTiming::EventType* s_ppc_bootstrap_done_event;
CoreTiming::EventType* s_ios_boot_done_event;

// Fields of the "OS globals" area in low MEM1 that the kernel fills in (WiiBrew, "Memory map").
namespace LowMem
{
// Copies kept for GameCube-era code.
constexpr u32 LEGACY_MEM_SIZE = 0x0028;
constexpr u32 LEGACY_ARENA_LO = 0x0030;
constexpr u32 LEGACY_ARENA_HI = 0x0034;
constexpr u32 LEGACY_SIM_MEM_SIZE = 0x00f0;

constexpr u32 MEM1_SIZE = 0x3100;
constexpr u32 MEM1_SIM_SIZE = 0x3104;
constexpr u32 MEM1_END = 0x3108;
constexpr u32 MEM1_ARENA_LO = 0x310c;
constexpr u32 MEM1_ARENA_HI = 0x3110;
constexpr u32 FILLER_1 = 0x3114;
constexpr u32 MEM2_SIZE = 0x3118;
constexpr u32 MEM2_SIM_SIZE = 0x311c;
constexpr u32 MEM2_END = 0x3120;
constexpr u32 MEM2_ARENA_LO = 0x3124;
constexpr u32 MEM2_ARENA_HI = 0x3128;
constexpr u32 FILLER_2 = 0x312c;
constexpr u32 IPC_BUF_LO = 0x3130;
constexpr u32 IPC_BUF_HI = 0x3134;
constexpr u32 HOLLYWOOD_REV = 0x3138;
constexpr u32 FILLER_3 = 0x313c;
constexpr u32 IOS_VERSION = 0x3140;
constexpr u32 IOS_BUILD_DATE = 0x3144;
constexpr u32 IOS_HEAP_LO = 0x3148;
constexpr u32 IOS_HEAP_HI = 0x314c;
constexpr u32 FILLER_4 = 0x3150;
constexpr u32 FILLER_5 = 0x3154;
constexpr u32 GDDR_VENDOR = 0x3158;
constexpr u32 BOOT_FLAG = 0x315c;
constexpr u32 APPLOADER_FLAG = 0x315d;
constexpr u32 DEVKIT_BOOT_VERSION = 0x315e;
constexpr u32 SYSMENU_SYNC = 0x3160;

constexpr u32 FILLER_WORD = 0xDEADBEEF;
}  // namespace LowMem

void WriteMem2Block(Memory::MemoryManager& memory, const MemoryValues& v)
{
  memory.Write_U32(v.mem2_physical_size, LowMem::MEM2_SIZE);
  memory.Write_U32(v.mem2_simulated_size, LowMem::MEM2_SIM_SIZE);
  memory.Write_U32(v.mem2_end, LowMem::MEM2_END);
  memory.Write_U32(v.mem2_arena_begin, LowMem::MEM2_ARENA_LO);
  memory.Write_U32(v.mem2_arena_end, LowMem::MEM2_ARENA_HI);
  memory.Write_U32(v.ipc_buffer_begin, LowMem::IPC_BUF_LO);
  memory.Write_U32(v.ipc_buffer_end, LowMem::IPC_BUF_HI);
  memory.Write_U32(v.ios_reserved_begin, LowMem::IOS_HEAP_LO);
  memory.Write_U32(v.ios_reserved_end, LowMem::IOS_HEAP_HI);
}

void WriteMem1Block(Memory::MemoryManager& memory, u32 physical, u32 simulated, u32 end,
                    u32 arena_lo, u32 arena_hi)
{
  memory.Write_U32(physical, LowMem::MEM1_SIZE);
  memory.Write_U32(simulated, LowMem::MEM1_SIM_SIZE);
  memory.Write_U32(end, LowMem::MEM1_END);
  memory.Write_U32(arena_lo, LowMem::MEM1_ARENA_LO);
  memory.Write_U32(arena_hi, LowMem::MEM1_ARENA_HI);

  memory.Write_U32(physical, LowMem::LEGACY_MEM_SIZE);
  memory.Write_U32(arena_lo, LowMem::LEGACY_ARENA_LO);
  memory.Write_U32(arena_hi, LowMem::LEGACY_ARENA_HI);
  memory.Write_U32(simulated, LowMem::LEGACY_SIM_MEM_SIZE);
}

const MemoryValues* FindMemoryValues(u64 ios_title_id)
{
  const u16 number = static_cast<u16>(ios_title_id & 0xffff);
  for (const MemoryValues& v : GetMemoryValues())
  {
    if (v.ios_number == number)
      return &v;
  }
  return nullptr;
}

bool SetupMemory(Memory::MemoryManager& memory, u64 ios_title_id, MemorySetupType setup_type)
{
  const MemoryValues* values = FindMemoryValues(ios_title_id);
  if (values == nullptr)
  {
    ERROR_LOG_FMT(IOS, "No memory layout known for IOS {:016x}", ios_title_id);
    return false;
  }

  if (setup_type == MemorySetupType::IOSReload)
  {
    // A reload only publishes the new version and the MEM2 split. Real kernels before IOS28
    // inherit the MEM2 values from whoever ran before them, and later ones rewrite them; the
    // end result is always the new kernel's own layout, which is what is written here.
    memory.Write_U32(values->ios_version, LowMem::IOS_VERSION);
    WriteMem2Block(memory, *values);
    RAMOverrideForIOSMemoryValues(memory, setup_type);
    return true;
  }

  // A full set-up starts from a cleared globals area (the last byte of the page is kept).
  memory.Memset(0, 0, 0x3fff);

  WriteMem1Block(memory, values->mem1_physical_size, values->mem1_simulated_size,
                 values->mem1_end, values->mem1_arena_begin, values->mem1_arena_end);
  WriteMem2Block(memory, *values);

  memory.Write_U32(values->hollywood_revision, LowMem::HOLLYWOOD_REV);
  memory.Write_U32(values->ios_version, LowMem::IOS_VERSION);
  memory.Write_U32(values->ios_date, LowMem::IOS_BUILD_DATE);
  memory.Write_U32(values->ram_vendor, LowMem::GDDR_VENDOR);
  memory.Write_U32(values->sysmenu_sync, LowMem::SYSMENU_SYNC);

  for (const u32 filler : {LowMem::FILLER_1, LowMem::FILLER_2, LowMem::FILLER_3,
                           LowMem::FILLER_4, LowMem::FILLER_5})
  {
    memory.Write_U32(LowMem::FILLER_WORD, filler);
  }
  // The three boot-status fields read as DE AD BE EF together.
  memory.Write_U8(0xDE, LowMem::BOOT_FLAG);
  memory.Write_U8(0xAD, LowMem::APPLOADER_FLAG);
  memory.Write_U16(0xBEEF, LowMem::DEVKIT_BOOT_VERSION);

  RAMOverrideForIOSMemoryValues(memory, setup_type);
  return true;
}

// While Starlet loads something for the PPC it keeps the PPC in reset. There is no reset line
// to hold here, so the CPU is reset and parked on a branch-to-self at address 0 instead. The
// PPC does not run while parked, so nothing can observe the branch instruction.
void HoldPPCInReset(Core::System& system)
{
  constexpr u32 BRANCH_TO_SELF = 0x48000000;
  system.GetMemory().Write_U32(BRANCH_TO_SELF, 0);
  auto& cpu = system.GetPowerPC();
  cpu.Reset();
  cpu.GetPPCState().pc = 0;
}

// Lets the PPC go again. A NAND title's boot stub would jump to 0x3400 with translation off;
// that code sets the machine state up itself, so only the PC needs to be placed.
void StartPPC(Core::System& system, bool is_ancast)
{
  system.GetMemory().Write_U32(0, 0);
  if (is_ancast)
  {
    // The image was already checked and decrypted by the loader; start right at its body.
    system.GetPPCState().pc = ESPRESSO_ANCAST_LOCATION_VIRT + sizeof(EspressoAncastHeader);
  }
  else
  {
    system.GetPPCState().pc = 0x3400;
  }
}

// Reads a whole NAND file through the FS core so that the access is timed. Returns nothing if
// the file cannot be opened, is larger than max_size (when non-zero), or a read returns 0.
std::vector<u8> LoadNandFile(FSCore& fs, const std::string& path, size_t max_size,
                             Ticks ticks = {})
{
  const auto handle = fs.Open(0, 0, path, FS::Mode::Read, {}, ticks);
  if (handle.Get() < 0)
    return {};
  const u64 fd = static_cast<u64>(handle.Get());

  const auto status = fs.GetFileStatus(fd, ticks);
  if (!status)
    return {};
  const size_t length = status->size;
  if (max_size != 0 && length > max_size)
    return {};

  std::vector<u8> contents(length);
  if (fs.Read(fd, contents.data(), contents.size(), ticks) == 0)
    return {};
  return contents;
}

// An IOS kernel image on the NAND: a small big-endian header (header size, ELF offset relative
// to the end of the header, ELF size) followed by the ELF itself.
class KernelImage
{
public:
  explicit KernelImage(std::vector<u8> data) : m_data(std::move(data)) {}

  bool IsValid() const
  {
    if (m_data.size() < 0x10)
      return false;
    // The sum is 32-bit, like the loader's.
    const u32 needed = HeaderSize() + ElfOffset() + ElfSize();
    return m_data.size() >= needed;
  }

  std::vector<u8> Elf() const
  {
    const size_t begin = size_t{HeaderSize()} + ElfOffset();
    return {m_data.begin() + static_cast<std::ptrdiff_t>(begin),
            m_data.begin() + static_cast<std::ptrdiff_t>(begin + ElfSize())};
  }

private:
  u32 Word(size_t offset) const { return Common::swap32(m_data.data() + offset); }
  u32 HeaderSize() const { return Word(0); }
  u32 ElfOffset() const { return Word(4); }
  u32 ElfSize() const { return Word(8); }

  std::vector<u8> m_data;
};

void SwitchKernel(Core::System& system, u64 ios_title_id)
{
  // The old kernel has to be gone before the new one is built (only one may exist at a time).
  system.SetIOS(nullptr);
  system.SetIOS(std::make_unique<EmulationKernel>(system, ios_title_id));
}

constexpr SystemTimers::TimeBaseTick KernelLoadTime(u32 version)
{
  // Kernels before IOS28 link all modules into one big image, which takes far longer to load.
  return version < 28 ? 16'000'000_tbticks : 2'600'000_tbticks;
}

void OnPPCBootstrapDone(Core::System& system, u64 userdata, s64)
{
  StartPPC(system, userdata == 1);

  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(system);
  SConfig::OnTitleDirectlyBooted(guard);

  INFO_LOG_FMT(IOS, "PPC bootstrap complete");
}
}  // namespace

void RAMOverrideForIOSMemoryValues(Memory::MemoryManager& memory, MemorySetupType setup_type)
{
  const bool override_wanted = Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE) ||
                               SConfig::GetInstance().GetSimulatedMemorySize() >
                                   Memory::MEM1_SIZE_RETAIL;
  if (!override_wanted)
    return;

  // Keep the IPC buffer and the IOS heap at their current sizes and move them to the new top
  // of MEM2; MEM1 is handed to the arena in full.
  const u32 ipc_size = memory.Read_U32(LowMem::IPC_BUF_HI) - memory.Read_U32(LowMem::IPC_BUF_LO);
  const u32 heap_size =
      memory.Read_U32(LowMem::IOS_HEAP_HI) - memory.Read_U32(LowMem::IOS_HEAP_LO);

  const u32 mem1_size = memory.GetRamSizeReal();
  const u32 mem2_size = memory.GetExRamSizeReal();
  const u32 mem1_top = Memory::MEM1_BASE_ADDR + mem1_size;
  const u32 mem2_top = Memory::MEM2_BASE_ADDR + mem2_size;
  const u32 heap_lo = mem2_top - heap_size;
  const u32 ipc_lo = heap_lo - ipc_size;

  // After a reload the game's apploader may already have adjusted the MEM1 values; leave them.
  if (setup_type == MemorySetupType::Full)
    WriteMem1Block(memory, mem1_size, mem1_size, mem1_top, 0, mem1_top);

  MemoryValues mem2{};
  mem2.mem2_physical_size = mem2_size;
  mem2.mem2_simulated_size = mem2_size;
  mem2.mem2_end = heap_lo;
  mem2.mem2_arena_begin = Memory::MEM2_BASE_ADDR + 0x800U;
  mem2.mem2_arena_end = ipc_lo;
  mem2.ipc_buffer_begin = ipc_lo;
  mem2.ipc_buffer_end = heap_lo;
  mem2.ios_reserved_begin = heap_lo;
  mem2.ios_reserved_end = mem2_top;
  WriteMem2Block(memory, mem2);
}

void WriteReturnValue(Memory::MemoryManager& memory, s32 value, u32 address)
{
  memory.Write_U32(static_cast<u32>(value), address);
}

Kernel::Kernel(IOSC::ConsoleType console_type) : m_iosc(console_type)
{
  // The NAND root is still process-wide state, so two kernels cannot coexist.
  ASSERT(Core::System::GetInstance().GetIOS() == nullptr);

  if (!Core::WiiRootIsInitialized())
  {
    m_is_responsible_for_nand_root = true;
    Core::InitializeWiiRoot(false);
  }

  m_fs = FS::MakeFileSystem(IOS::HLE::FS::Location::Session, Core::GetActiveNandRedirects());
  ASSERT(m_fs);

  m_fs_core = std::make_unique<FSCore>(*this);
  m_es_core = std::make_unique<ESCore>(*this);
}

Kernel::~Kernel()
{
  if (m_is_responsible_for_nand_root)
    Core::ShutdownWiiRoot();
}

Kernel::Kernel(u64 title_id) : m_title_id(title_id)
{
}

EmulationKernel::EmulationKernel(Core::System& system, u64 title_id)
    : Kernel(title_id), m_system(system)
{
  INFO_LOG_FMT(IOS, "Booting IOS {:016x}", title_id);

  if (!SetupMemory(m_system.GetMemory(), title_id, MemorySetupType::IOSReload))
    WARN_LOG_FMT(IOS, "Low memory values were left as they were");

  // MIOS replaces IOS entirely; it has no resource managers.
  if (title_id == Titles::MIOS)
  {
    MIOS::Load(m_system);
    return;
  }

  m_fs = FS::MakeFileSystem(IOS::HLE::FS::Location::Session, Core::GetActiveNandRedirects());
  ASSERT(m_fs);

  AddDevice(std::make_unique<AesDevice>(*this, "/dev/aes"));
  AddDevice(std::make_unique<ShaDevice>(*this, "/dev/sha"));

  m_fs_core = std::make_unique<FSCore>(*this);
  AddDevice(std::make_unique<FSDevice>(*this, *m_fs_core, "/dev/fs"));
  m_es_core = std::make_unique<ESCore>(*this);
  AddDevice(std::make_unique<ESDevice>(*this, *m_es_core, "/dev/es"));

  AddStaticDevices();
}

EmulationKernel::~EmulationKernel()
{
  m_system.GetCoreTiming().RemoveAllEvents(s_ipc_event);

  m_device_map.clear();
  m_socket_manager.reset();
}

u32 Kernel::GetVersion() const
{
  // An IOS title ID is 00000001-000000NN; the low word is the major version.
  return static_cast<u32>(m_title_id);
}

std::shared_ptr<FS::FileSystem> Kernel::GetFS()
{
  return m_fs;
}

FSCore& Kernel::GetFSCore()
{
  return *m_fs_core;
}

ESCore& Kernel::GetESCore()
{
  return *m_es_core;
}

IOSC& Kernel::GetIOSC()
{
  return m_iosc;
}

std::shared_ptr<FSDevice> EmulationKernel::GetFSDevice()
{
  return std::static_pointer_cast<FSDevice>(m_device_map.at("/dev/fs"));
}

std::shared_ptr<ESDevice> EmulationKernel::GetESDevice()
{
  return std::static_pointer_cast<ESDevice>(m_device_map.at("/dev/es"));
}

std::shared_ptr<WiiSockMan> EmulationKernel::GetSocketManager()
{
  return m_socket_manager;
}

// Only the PPC's credentials are tracked, since there are no real IOS processes. These stand in
// for the set/get UID and GID syscalls.
void EmulationKernel::SetUidForPPC(u32 uid)
{
  m_ppc_uid = uid;
}

u32 EmulationKernel::GetUidForPPC() const
{
  return m_ppc_uid;
}

void EmulationKernel::SetGidForPPC(u16 gid)
{
  m_ppc_gid = gid;
}

u16 EmulationKernel::GetGidForPPC() const
{
  return m_ppc_gid;
}

// Loads a DOL (or Ancast image) from the NAND, publishes the low memory values and lets the PPC
// run it once the load time has passed. This is the "launch PPC title" syscall.
bool EmulationKernel::BootstrapPPC(const std::string& boot_content_path)
{
  // Only the NAND read is timed; everything else is cheap in comparison.
  u64 load_ticks = 0;
  const DolReader dol{LoadNandFile(GetFSCore(), boot_content_path, 0, &load_ticks)};
  if (!dol.IsValid())
    return false;

  if (!SetupMemory(m_system.GetMemory(), m_title_id, MemorySetupType::Full))
    return false;

  HoldPPCInReset(m_system);

  const bool ancast = dol.IsAncast();
  if (ancast)
    INFO_LOG_FMT(IOS, "PPC bootstrap: image is Ancast");

  if (!dol.LoadIntoMemory(m_system))
    return false;

  INFO_LOG_FMT(IOS, "PPC bootstrap: loaded {}", boot_content_path);
  m_system.GetCoreTiming().ScheduleEvent(static_cast<s64>(load_ticks), s_ppc_bootstrap_done_event, ancast);
  return true;
}

// Replaces the running kernel. Like the real syscall this only changes the version word in low
// memory (the new kernel's constructor writes it), and it tears this instance down.
//
// The boot content is optional since IOS images do not have to be installed. When a path is
// given the image has to exist and be well formed, as on hardware.
bool EmulationKernel::BootIOS(const u64 ios_title_id, HangPPC hang_ppc,
                              const std::string& boot_content_path)
{
  // IPC stays stopped from here on; it is only restarted by the new kernel.
  m_ipc_paused = true;

  if (!boot_content_path.empty())
  {
    // Starlet is not emulated, so only the parts of the ELF that land in MEM1 matter.
    const KernelImage image{LoadNandFile(GetFSCore(), boot_content_path, 0xB00000)};
    if (!image.IsValid())
      return false;

    ElfReader elf{image.Elf()};
    if (!elf.LoadIntoMemory(m_system, true))
      return false;
  }

  if (hang_ppc == HangPPC::Yes)
    HoldPPCInReset(m_system);

  if (!Core::IsRunning(m_system))
  {
    SwitchKernel(m_system, ios_title_id);
    return true;
  }

  m_system.GetCoreTiming().ScheduleEvent(static_cast<s64>(KernelLoadTime(GetVersion())),
                                         s_ios_boot_done_event, ios_title_id);
  return true;
}

void EmulationKernel::InitIPC()
{
  if (Core::IsUninitialized(m_system))
    return;

  INFO_LOG_FMT(IOS, "IPC ready");
  m_system.GetWiiIPC().GenerateAck(0);
}

void EmulationKernel::AddDevice(std::unique_ptr<Device> device)
{
  ASSERT(device->GetDeviceType() == Device::DeviceType::Static);
  const std::string name = device->GetDeviceName();
  m_device_map.insert_or_assign(name, std::move(device));
}

void EmulationKernel::AddStaticDevices()
{
  const Feature features = GetFeatures(GetVersion());
  const auto has = [features](Feature f) { return HasFeature(features, f); };

  // Emulator-only interface for homebrew.
  AddDevice(std::make_unique<DolphinDevice>(*this, "/dev/dolphin"));

  // Bluetooth (OH1): the bus node is a stub, the Wii Remote adapter is real or emulated.
  AddDevice(std::make_unique<DeviceStub>(*this, "/dev/usb/oh1"));
  constexpr const char* bt_path = "/dev/usb/oh1/57e/305";
  if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    AddDevice(std::make_unique<BluetoothRealDevice>(*this, bt_path));
  else
    AddDevice(std::make_unique<BluetoothEmuDevice>(*this, bt_path));

  AddDevice(std::make_unique<STMImmediateDevice>(*this, "/dev/stm/immediate"));
  AddDevice(std::make_unique<STMEventHookDevice>(*this, "/dev/stm/eventhook"));
  AddDevice(std::make_unique<DIDevice>(*this, "/dev/di"));
  AddDevice(std::make_unique<SDIOSlot0Device>(*this, "/dev/sdio/slot0"));
  AddDevice(std::make_unique<DeviceStub>(*this, "/dev/sdio/slot1"));

  // Networking. The socket manager is shared by KD, IP and SSL.
  if (has(Feature::KD) || has(Feature::SO) || has(Feature::SSL))
    m_socket_manager = std::make_shared<IOS::HLE::WiiSockMan>(*this);

  if (has(Feature::KD))
  {
    constexpr const char* kd_time_path = "/dev/net/kd/time";
    AddDevice(std::make_unique<NetKDTimeDevice>(*this, kd_time_path));
    auto kd_time = std::static_pointer_cast<NetKDTimeDevice>(GetDeviceByName(kd_time_path));
    AddDevice(std::make_unique<NetKDRequestDevice>(*this, "/dev/net/kd/request", kd_time));
  }
  if (has(Feature::NCD))
    AddDevice(std::make_unique<NetNCDManageDevice>(*this, "/dev/net/ncd/manage"));
  if (has(Feature::WiFi))
    AddDevice(std::make_unique<NetWDCommandDevice>(*this, "/dev/net/wd/command"));
  if (has(Feature::SO))
    AddDevice(std::make_unique<NetIPTopDevice>(*this, "/dev/net/ip/top"));
  if (has(Feature::SSL))
    AddDevice(std::make_unique<NetSSLDevice>(*this, "/dev/net/ssl"));

  // USB. /dev/usb/oh0 exists in every version.
  AddDevice(std::make_unique<OH0>(*this, "/dev/usb/oh0"));
  if (has(Feature::NewUSB))
  {
    // The new stack also has usb, msc, hub and ehc nodes; those are not provided yet.
    AddDevice(std::make_unique<USB_HIDv5>(*this, "/dev/usb/hid"));
    AddDevice(std::make_unique<USB_VEN>(*this, "/dev/usb/ven"));
  }
  else
  {
    if (has(Feature::USB_HIDv4))
      AddDevice(std::make_unique<USB_HIDv4>(*this, "/dev/usb/hid"));
    if (has(Feature::USB_KBD))
      AddDevice(std::make_unique<USB_KBD>(*this, "/dev/usb/kbd"));
  }

  if (has(Feature::WFS))
  {
    AddDevice(std::make_unique<WFSSRVDevice>(*this, "/dev/usb/wfssrv"));
    AddDevice(std::make_unique<WFSIDevice>(*this, "/dev/wfsi"));
  }
}

s32 EmulationKernel::GetFreeDeviceID()
{
  const auto slot = std::ranges::find(m_fdmap, nullptr);
  if (slot == m_fdmap.end())
    return -1;
  return static_cast<s32>(slot - m_fdmap.begin());
}

std::shared_ptr<Device> EmulationKernel::GetDeviceByName(std::string_view device_name)
{
  if (const auto found = m_device_map.find(device_name); found != m_device_map.end())
    return found->second;
  return nullptr;
}

std::shared_ptr<Device> EmulationKernel::GetDeviceByFileDescriptor(const u32 fd)
{
  if (fd < IPC_MAX_FDS)
    return m_fdmap[fd];

  // The crypto engines are reachable through two fixed descriptors without an Open.
  constexpr u32 AES_FD = 0x10000;
  constexpr u32 SHA_FD = 0x10001;
  if (fd == AES_FD)
    return GetDeviceByName("/dev/aes");
  if (fd == SHA_FD)
    return GetDeviceByName("/dev/sha");
  return nullptr;
}

// Picks a free descriptor, finds the target and opens it. On success the reply carries the new
// descriptor; otherwise an error.
std::optional<IPCReply> EmulationKernel::OpenDevice(OpenRequest& request)
{
  const s32 fd = GetFreeDeviceID();
  INFO_LOG_FMT(IOS, "Open {} with mode {} -> fd {}", request.path,
               std::to_underlying(request.flags), fd);
  if (fd < 0 || fd >= IPC_MAX_FDS)
  {
    ERROR_LOG_FMT(IOS, "Descriptor table is full");
    return IPCReply{IPC_EMAX, 5000_tbticks};
  }
  request.fd = static_cast<u32>(fd);

  const std::string& path = request.path;
  std::shared_ptr<Device> target;
  if (path.starts_with("/dev/usb/oh0/") && !GetDeviceByName(path) &&
      !HasFeature(GetVersion(), Feature::NewUSB))
  {
    // Old USB stack: each device under oh0 gets its own node, made on demand.
    target = std::make_shared<OH0Device>(*this, path);
  }
  else if (path.starts_with("/dev/"))
  {
    target = GetDeviceByName(path);
  }
  else if (path.starts_with('/'))
  {
    // Any other absolute path is a NAND file, which the FS module serves.
    target = GetDeviceByName("/dev/fs");
  }

  if (target == nullptr)
  {
    // Device nodes of custom IOS builds; titles often probe these to detect them.
    static constexpr std::array<std::string_view, 5> custom_ios_nodes{
        "/dev/flash", "/dev/mload", "/dev/sdio/sdhc", "/dev/usb123", "/dev/usb2"};
    if (std::ranges::find(custom_ios_nodes, std::string_view{path}) != custom_ios_nodes.end())
      WARN_LOG_FMT(IOS, "{} belongs to a custom IOS (probably an anti-piracy check)", path);
    else
      ERROR_LOG_FMT(IOS, "No such device: {}", path);
    return IPCReply{IPC_ENOENT, 3700_tbticks};
  }

  std::optional<IPCReply> reply = target->Open(request);
  if (reply.has_value() && reply->return_value >= IPC_SUCCESS)
  {
    m_fdmap[static_cast<u32>(fd)] = target;
    reply->return_value = fd;
  }
  return reply;
}

std::optional<IPCReply> EmulationKernel::HandleIPCCommand(const Request& request)
{
  const IPCCommandType command = request.command;
  if (command < IPC_CMD_OPEN || command > IPC_CMD_IOCTLV)
    return IPCReply{IPC_EINVAL, 978_tbticks};

  if (command == IPC_CMD_OPEN)
  {
    OpenRequest open{GetSystem(), request.address};
    return OpenDevice(open);
  }

  const std::shared_ptr<Device> device = GetDeviceByFileDescriptor(request.fd);
  if (device == nullptr)
    return IPCReply{IPC_EINVAL, 550_tbticks};

  auto& system = GetSystem();
  const u64 started_us = Common::Timer::NowUs();

  std::optional<IPCReply> reply;
  if (command == IPC_CMD_CLOSE)
  {
    // Free the slot first; the fixed crypto descriptors have no slot.
    if (request.fd < IPC_MAX_FDS)
      m_fdmap[request.fd].reset();
    reply = device->Close(request.fd);
  }
  else if (command == IPC_CMD_READ)
  {
    reply = device->Read(ReadWriteRequest{system, request.address});
  }
  else if (command == IPC_CMD_WRITE)
  {
    reply = device->Write(ReadWriteRequest{system, request.address});
  }
  else if (command == IPC_CMD_SEEK)
  {
    reply = device->Seek(SeekRequest{system, request.address});
  }
  else if (command == IPC_CMD_IOCTL)
  {
    reply = device->IOCtl(IOCtlRequest{system, request.address});
  }
  else
  {
    reply = device->IOCtlV(IOCtlVRequest{system, request.address});
  }

  // Report requests that held up the emulation thread for a noticeable time.
  constexpr u64 SLOW_REQUEST_US = 2000;
  const u64 elapsed_us = Common::Timer::NowUs() - started_us;
  if (elapsed_us > SLOW_REQUEST_US)
  {
    WARN_LOG_FMT(IOS, "A request to {} took {} us of host time", device->GetDeviceName(),
                 elapsed_us);
  }

  return reply;
}

void EmulationKernel::ExecuteIPCCommand(const u32 address)
{
  const Request request{GetSystem(), address};
  std::optional<IPCReply> reply = HandleIPCCommand(request);

  // Devices that answer later (asynchronously) return nothing here.
  if (!reply)
    return;

  // Replies must not overtake each other, so never schedule one before the previous one.
  const u64 now = GetSystem().GetCoreTiming().GetTicks();
  const s64 wait_for_previous = static_cast<s64>(m_last_reply_time - now);
  if (wait_for_previous > 0)
    reply->reply_delay_ticks += static_cast<u64>(wait_for_previous);
  m_last_reply_time = now + reply->reply_delay_ticks;

  EnqueueIPCReply(request, reply->return_value, static_cast<s64>(reply->reply_delay_ticks));
}

// Called when the PPC writes a new request pointer.
void EmulationKernel::EnqueueIPCRequest(u32 address)
{
  // Measured on hardware, the acknowledge comes about 450 to 660 timebase ticks later.
  GetSystem().GetCoreTiming().ScheduleEvent(500_tbticks, s_ipc_event,
                                            ENQUEUE_REQUEST_FLAG | address);
}

void EmulationKernel::EnqueueIPCReply(const Request& request, const s32 return_value,
                                      s64 cycles_in_future, CoreTiming::FromThread from)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  // Reply block: result at +4, the original command moved into the fd slot at +8, and the
  // command word itself marked as a reply.
  memory.Write_U32(static_cast<u32>(return_value), request.address + 4);
  memory.Write_U32(request.command, request.address + 8);
  memory.Write_U32(IPC_REPLY, request.address);

  system.GetCoreTiming().ScheduleEvent(cycles_in_future, s_ipc_event, request.address, from);
}

void EmulationKernel::HandleIPCEvent(u64 userdata)
{
  const u32 address = static_cast<u32>(userdata);
  auto& queue = (userdata & ENQUEUE_REQUEST_FLAG) != 0 ? m_request_queue : m_reply_queue;
  queue.push_back(address);
  UpdateIPC();
}

// Moves at most one message across: a pending request takes priority over a pending reply.
void EmulationKernel::UpdateIPC()
{
  auto& ipc = m_system.GetWiiIPC();
  if (m_ipc_paused || !ipc.IsReady())
    return;

  if (!m_request_queue.empty())
  {
    const u32 address = m_request_queue.front();
    m_request_queue.pop_front();
    ipc.ClearX1();
    ipc.GenerateAck(address);
    ExecuteIPCCommand(address);
    return;
  }

  if (!m_reply_queue.empty())
  {
    const u32 address = m_reply_queue.front();
    m_reply_queue.pop_front();
    ipc.GenerateReply(address);
    DEBUG_LOG_FMT(IOS, "Reply delivered for request at {:#010x}", address);
  }
}

void EmulationKernel::UpdateDevices()
{
  for (const auto& entry : m_device_map)
  {
    if (entry.second->IsOpened())
      entry.second->Update();
  }
}

void EmulationKernel::UpdateWantDeterminism(const bool new_want_determinism)
{
  if (m_socket_manager)
    m_socket_manager->UpdateWantDeterminism(new_want_determinism);
  for (const auto& entry : m_device_map)
    entry.second->UpdateWantDeterminism(new_want_determinism);
}

void EmulationKernel::DoState(PointerWrap& p)
{
  p.Do(m_request_queue);
  p.Do(m_reply_queue);
  p.Do(m_last_reply_time);
  p.Do(m_ipc_paused);
  p.Do(m_title_id);
  p.Do(m_ppc_uid);
  p.Do(m_ppc_gid);

  m_iosc.DoState(p);
  m_fs->DoState(p);

  // MIOS has no devices to save.
  if (m_title_id == Titles::MIOS)
    return;

  if (m_socket_manager)
    m_socket_manager->DoState(p);

  for (const auto& entry : m_device_map)
    entry.second->DoState(p);

  // The descriptor table: per slot a u32 "in use" flag, then the device type; static devices are
  // stored by name, OH0 child nodes save their whole state.
  const bool loading = p.IsReadMode();
  for (std::shared_ptr<Device>& slot : m_fdmap)
  {
    u32 in_use = (!loading && slot) ? 1 : 0;
    p.Do(in_use);
    if (in_use == 0)
      continue;

    Device::DeviceType type = loading ? Device::DeviceType::Static : slot->GetDeviceType();
    p.Do(type);

    if (type == Device::DeviceType::Static)
    {
      std::string name = loading ? std::string{} : slot->GetDeviceName();
      p.Do(name);
      if (loading)
        slot = GetDeviceByName(name);
    }
    else if (type == Device::DeviceType::OH0)
    {
      if (loading)
        slot = std::make_shared<OH0Device>(*this, "");
      slot->DoState(p);
    }
  }
}

void Init(Core::System& system)
{
  auto& timing = system.GetCoreTiming();

  s_ipc_event = timing.RegisterEvent("IPCEvent", [](Core::System& sys, u64 userdata, s64) {
    if (EmulationKernel* kernel = sys.GetIOS(); kernel != nullptr)
      kernel->HandleIPCEvent(userdata);
  });

  ESDevice::InitializeEmulationState(timing);

  s_ppc_bootstrap_done_event = timing.RegisterEvent("IOSFinishPPCBootstrap", OnPPCBootstrapDone);

  s_ios_boot_done_event = timing.RegisterEvent(
      "IOSFinishIOSBoot",
      [](Core::System& sys, u64 ios_title_id, s64) { SwitchKernel(sys, ios_title_id); });

  DIDevice::s_finish_executing_di_command =
      timing.RegisterEvent("FinishDICommand", DIDevice::FinishDICommandCallback);

  // A console boots into the System Menu's IOS, and the System Menu's launch fills in the low
  // memory values. Booting a game directly skips that, so do the same set-up here.
  system.SetIOS(std::make_unique<EmulationKernel>(system, Titles::SYSTEM_MENU_IOS));
  SetupMemory(system.GetMemory(), Titles::SYSTEM_MENU_IOS, MemorySetupType::Full);
}

void Shutdown(Core::System& system)
{
  system.SetIOS(nullptr);
  ESDevice::FinalizeEmulationState();
}

// Hardware answers in about 2700 ticks at best and often takes 10000 or more when the NAND is
// involved. Timing is approximate anyway, so the default is a middle value rather than the
// minimum, to avoid running much faster than a console.
IPCReply::IPCReply(s32 return_value_) : IPCReply(return_value_, 4000_tbticks)
{
}

IPCReply::IPCReply(s32 return_value_, u64 reply_delay_ticks_)
    : return_value(return_value_), reply_delay_ticks(reply_delay_ticks_)
{
}
}  // namespace IOS::HLE
