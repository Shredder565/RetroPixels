// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/UCodes/UCodes.h"

#include <memory>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/DSP/DSPCodeUtil.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/UCodes/AESnd.h"
#include "Core/HW/DSPHLE/UCodes/ASnd.h"
#include "Core/HW/DSPHLE/UCodes/AX.h"
#include "Core/HW/DSPHLE/UCodes/AXWii.h"
#include "Core/HW/DSPHLE/UCodes/CARD.h"
#include "Core/HW/DSPHLE/UCodes/GBA.h"
#include "Core/HW/DSPHLE/UCodes/INIT.h"
#include "Core/HW/DSPHLE/UCodes/ROM.h"
#include "Core/HW/DSPHLE/UCodes/Zelda.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace DSP::HLE
{
UCodeInterface::UCodeInterface(DSPHLE* dsphle, u32 crc)
    : m_mail_handler(dsphle->AccessMailHandler()), m_dsphle(dsphle), m_crc(crc)
{
}

UCodeInterface::~UCodeInterface() = default;

bool UCodeInterface::NeedsResumeMail()
{
  const bool pending = m_needs_resume_mail;
  m_needs_resume_mail = false;
  return pending;
}

void UCodeInterface::PrepareBootUCode(u32 mail)
{
  const u16 low = static_cast<u16>(mail);
  BootRequest& req = m_boot_request;

  // The mails come in a fixed order: DRAM -> MRAM save, MRAM -> IRAM load, MRAM -> DRAM load.
  switch (m_boot_mails_received)
  {
  case 0:
    req.mram_dest_addr = mail;
    break;
  case 1:
    req.mram_size = low;
    break;
  case 2:
    req.mram_dram_addr = low;
    break;
  case 3:
    req.iram_mram_addr = mail;
    break;
  case 4:
    req.iram_size = low;
    break;
  case 5:
    req.iram_dest = low;
    break;
  case 6:
    req.iram_startpc = low;
    break;
  case 7:
    req.dram_mram_addr = mail;
    break;
  case 8:
    req.dram_size = low;
    break;
  case 9:
    req.dram_dest = low;
    break;
  default:
    break;
  }

  if (++m_boot_mails_received != 10)
    return;

  m_boot_mails_received = 0;
  m_needs_resume_mail = true;
  m_upload_setup_in_progress = false;

  const u32 crc = IdentifyUCodeImage(m_dsphle, req.iram_mram_addr, req.iram_size);

  DEBUG_LOG_FMT(DSPHLE, "Task switch to ucode {:08x}", crc);
  DEBUG_LOG_FMT(DSPHLE, "  save DRAM {:04x} -> RAM {:08x}, {:04x} bytes", req.mram_dram_addr,
                req.mram_dest_addr, req.mram_size);
  DEBUG_LOG_FMT(DSPHLE, "  load RAM {:08x} -> IRAM {:04x}, {:04x} bytes, entry {:04x}",
                req.iram_mram_addr, req.iram_dest, req.iram_size, req.iram_startpc);
  DEBUG_LOG_FMT(DSPHLE, "  load RAM {:08x} -> DRAM {:04x}, {:04x} bytes", req.dram_mram_addr,
                req.dram_dest, req.dram_size);

  // HLE ucodes keep their state in host memory, so DRAM transfers have nothing to act on.
  if (req.mram_size != 0)
    WARN_LOG_FMT(DSPHLE, "Task switch saves DRAM to RAM; not emulated in HLE");
  if (req.dram_size != 0)
    WARN_LOG_FMT(DSPHLE, "Task switch loads DRAM from RAM; not emulated in HLE");

  // May destroy this object: nothing may touch members afterwards.
  m_dsphle->SwapUCode(crc);
}

void UCodeInterface::DoStateShared(PointerWrap& p)
{
  p.Do(m_upload_setup_in_progress);
  p.Do(m_boot_request);
  p.Do(m_boot_mails_received);
  p.Do(m_needs_resume_mail);
}

u32 IdentifyUCodeImage(DSPHLE* dsphle, u32 address, u32 size)
{
  const auto& memory = dsphle->GetSystem().GetMemory();
  const u8* image = memory.GetPointerForRange(address, size);
  const u32 crc = Common::HashEctor(image, size);

  if (Config::Get(Config::MAIN_DUMP_UCODE))
    DumpDSPCode(image, size, crc);

  return crc;
}

namespace
{
enum class Family
{
  ROM,
  Init,
  Card,
  GBA,
  AX,
  AXWii,
  Zelda,
  ASnd,
  AESnd,
};

struct KnownUCode
{
  u32 crc;
  Family family;
};

// Every ucode recognised by CRC. Comments name some of the software using each one.
constexpr KnownUCode KNOWN_UCODES[] = {
    {UCODE_ROM, Family::ROM},
    {UCODE_INIT_AUDIO_SYSTEM, Family::Init},
    {0x65d6cc6f, Family::Card},
    {0xdd7e72d5, Family::GBA},

    // GameCube AX.
    {0x3ad3b7ac, Family::AX},  // Naruto 3, Paper Mario: The Thousand-Year Door
    {0x3daf59b9, Family::AX},  // Alien Hominid
    {0x4e8a8b21, Family::AX},  // the oldest AX: Crazy Taxi, Super Monkey Ball, Smash Bros., ...
    {0x07f88145, Family::AX},  // F-Zero GX, Ikaruga, Soul Calibur 2, Zelda: Ocarina of Time, ...
    {0xe2136399, Family::AX},  // Billy Hatcher, Mario Party 5, 1080 Avalanche, ...
    {0x3389a79e, Family::AX},  // Metroid Prime 1/2 in Metroid Prime Trilogy (Wii)

    // The Zelda ("DAC") family.
    {0x86840740, Family::Zelda},  // Zelda: The Wind Waker
    {0x6ca33a6d, Family::Zelda},  // Zelda: Twilight Princess (GameCube)
    {0xd643001f, Family::Zelda},  // Super Mario Galaxy
    {0x6ba3b3ea, Family::Zelda},  // GameCube IPL, PAL
    {0x24b22038, Family::Zelda},  // GameCube IPL, NTSC
    {0x2fcdf1ec, Family::Zelda},  // Zelda: Four Swords Adventures
    {0xdf059f68, Family::Zelda},  // Pikmin demo
    {0x4be6a5cb, Family::Zelda},  // Pikmin (NTSC)
    {0x267fd05a, Family::Zelda},  // Pikmin (PAL)
    {0x42f64ac4, Family::Zelda},  // Luigi's Mansion
    {0x56d36052, Family::Zelda},  // Super Mario Sunshine
    {0x6c3f6f94, Family::Zelda},  // Zelda: Twilight Princess (Wii)
    {0xb7eb9a9c, Family::Zelda},  // Pikmin (New Play Control)
    {0xeaeb38cc, Family::Zelda},  // Pikmin 2 (New Play Control)

    // Wii AX.
    {0x2ea36ce6, Family::AXWii},  // some Wii demos
    {0x5ef56da3, Family::AXWii},  // AX demo
    {0x347112ba, Family::AXWii},  // Rayman Raving Rabbids
    {0xfa450138, Family::AXWii},  // Wii Sports (PAL)
    {0xadbc06bd, Family::AXWii},  // Elebits
    {0x4cc52064, Family::AXWii},  // Bleach: Versus Crusade
    {0xd9c4bf34, Family::AXWii},  // Wii System Menu 1.0
    {0x7699af32, Family::AXWii},  // Wii startup menu

    // Homebrew mixers from libogc and its forks.
    {ASndUCode::HASH_2008, Family::ASnd},
    {ASndUCode::HASH_2009, Family::ASnd},
    {ASndUCode::HASH_2011, Family::ASnd},
    {ASndUCode::HASH_2020, Family::ASnd},
    {ASndUCode::HASH_2020_PAD, Family::ASnd},
    {ASndUCode::HASH_DESERT_BUS_2011, Family::ASnd},
    {ASndUCode::HASH_DESERT_BUS_2012, Family::ASnd},
    {ASndUCode::HASH_2024, Family::ASnd},
    {ASndUCode::HASH_2024_PAD, Family::ASnd},
    {AESndUCode::HASH_2010, Family::AESnd},
    {AESndUCode::HASH_2012, Family::AESnd},
    {AESndUCode::HASH_EDUKE32, Family::AESnd},
    {AESndUCode::HASH_2020, Family::AESnd},
    {AESndUCode::HASH_2020_PAD, Family::AESnd},
    {AESndUCode::HASH_2022_PAD, Family::AESnd},
    {AESndUCode::HASH_2023, Family::AESnd},
};

const KnownUCode* FindUCode(u32 crc)
{
  for (const KnownUCode& entry : KNOWN_UCODES)
  {
    if (entry.crc == crc)
      return &entry;
  }
  return nullptr;
}
}  // namespace

std::unique_ptr<UCodeInterface> UCodeFactory(u32 crc, DSPHLE* dsphle, bool wii)
{
  if (crc == UCODE_NULL)
    return nullptr;

  const KnownUCode* known = FindUCode(crc);
  if (known == nullptr)
  {
    if (wii)
    {
      PanicAlertFmtT(
          "This title might be incompatible with DSP HLE emulation. Try using LLE if this "
          "is homebrew.\n\n"
          "Unknown ucode (CRC = {0:08x}) - forcing AXWii.",
          crc);
      return std::make_unique<AXWiiUCode>(dsphle, crc);
    }

    PanicAlertFmtT(
        "This title might be incompatible with DSP HLE emulation. Try using LLE if this "
        "is homebrew.\n\n"
        "DSPHLE: Unknown ucode (CRC = {0:08x}) - forcing AX.",
        crc);
    return std::make_unique<AXUCode>(dsphle, crc);
  }

  INFO_LOG_FMT(DSPHLE, "Starting HLE ucode {:08x}", crc);
  switch (known->family)
  {
  case Family::ROM:
    return std::make_unique<ROMUCode>(dsphle, crc);
  case Family::Init:
    return std::make_unique<INITUCode>(dsphle, crc);
  case Family::Card:
    return std::make_unique<CARDUCode>(dsphle, crc);
  case Family::GBA:
    return std::make_unique<GBAUCode>(dsphle, crc);
  case Family::AX:
    return std::make_unique<AXUCode>(dsphle, crc);
  case Family::AXWii:
    return std::make_unique<AXWiiUCode>(dsphle, crc);
  case Family::Zelda:
    return std::make_unique<ZeldaUCode>(dsphle, crc);
  case Family::ASnd:
    return std::make_unique<ASndUCode>(dsphle, crc);
  case Family::AESnd:
    return std::make_unique<AESndUCode>(dsphle, crc);
  }
  return nullptr;
}
}  // namespace DSP::HLE
