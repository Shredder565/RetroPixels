// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/UCodes/ROM.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace DSP::HLE
{
namespace
{
// Parameter selectors sent by the CPU, each followed by the value mail.
constexpr u32 PARAM_RAM_ADDRESS = 0x80F3A001;
constexpr u32 PARAM_IRAM_LENGTH = 0x80F3A002;
constexpr u32 PARAM_DRAM_LENGTH = 0x80F3B002;
constexpr u32 PARAM_IRAM_ADDRESS = 0x80F3C002;
constexpr u32 PARAM_START_PC = 0x80F3D001;
}  // namespace

ROMUCode::ROMUCode(DSPHLE* dsphle, u32 crc) : UCodeInterface(dsphle, crc)
{
  INFO_LOG_FMT(DSPHLE, "DSP boot ROM ready");
}

void ROMUCode::Initialize()
{
  m_mail_handler.PushMail(0x8071FEED);
}

void ROMUCode::Update()
{
}

void ROMUCode::HandleMail(u32 mail)
{
  if (m_pending_parameter == 0)
  {
    // Anything that is not a parameter selector is echoed back as 0xFEEExxxx.
    if ((mail & 0xFFFF0000) == 0x80F30000)
      m_pending_parameter = mail;
    else
      m_mail_handler.PushMail(0xFEEE0000 | (mail & 0xFFFF));
    return;
  }

  const u32 selector = m_pending_parameter;
  const u32 low = mail & 0xFFFF;
  switch (selector)
  {
  case PARAM_RAM_ADDRESS:
    m_params.ram_address = mail;
    break;
  case PARAM_IRAM_LENGTH:
    m_params.length = low;
    break;
  case PARAM_DRAM_LENGTH:
    m_params.dmem_length = low;
    if (low != 0)
      NOTICE_LOG_FMT(DSPHLE, "ROM boot with a DRAM image of {:#06x} bytes", low);
    break;
  case PARAM_IRAM_ADDRESS:
    m_params.imem_address = low;
    break;
  case PARAM_START_PC:
    m_params.start_pc = low;
    // Replaces this ucode: return without touching any member.
    StartLoadedUCode();
    return;
  default:
    break;
  }
  m_pending_parameter = 0;
}

void ROMUCode::StartLoadedUCode()
{
  const u32 crc = IdentifyUCodeImage(m_dsphle, m_params.ram_address, m_params.length);

  INFO_LOG_FMT(DSPHLE,
               "ROM boot: RAM {:08x}, {:#x} bytes to IRAM {:04x}, DRAM {:#x} bytes, entry {:04x}, "
               "crc {:08x}",
               m_params.ram_address, m_params.length, m_params.imem_address, m_params.dmem_length,
               m_params.start_pc, crc);

  m_dsphle->SetUCode(crc);
}

void ROMUCode::DoState(PointerWrap& p)
{
  p.Do(m_params);
  p.Do(m_unused_step_count);
  p.Do(m_pending_parameter);
  DoStateShared(p);
}
}  // namespace DSP::HLE
