// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/UCodes/GBA.h"

#include "Common/Align.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace DSP::HLE
{
namespace
{
// The two words of the BIOS cipher's author, "Kawasedo", used to wrap the results.
constexpr u32 WORD_SEDO = 0x6f646573;  // "sedo"
constexpr u32 WORD_KAWA = 0x6177614b;  // "Kawa"

// Palette and animation speed of the logo, as the GBA BIOS expects them.
u16 EncodeLogoAnimation(u32 palette, u32 raw_speed)
{
  const s32 speed = static_cast<s8>(raw_speed);
  if (speed == 0)
    return static_cast<u16>((palette * 2) | 0x70);

  const s32 magnitude_code = speed < 0 ? 2 - speed : speed - 1;
  return static_cast<u16>((palette << 4) | static_cast<u32>(magnitude_code * 2));
}
}  // namespace

void ProcessGBACrypto(Memory::MemoryManager& memory, u32 address)
{
  const u32 challenge = memory.Read_U32_Swap(address);
  const u32 palette = memory.Read_U32(address + 4);
  const u32 raw_speed = memory.Read_U32(address + 8);
  const u32 program_length = memory.Read_U32(address + 12);
  const u32 result_addr = memory.Read_U32(address + 16);

  const u32 session_key = challenge ^ WORD_SEDO;
  memory.Write_U32(session_key, result_addr);

  // The program goes over JoyBus in 4-byte packets, and the BIOS counts it in pairs of them,
  // not counting the 0x200-byte header.
  const s32 body_length = static_cast<s32>(Common::AlignUp(program_length, 8) - 0x200);
  const u16 packet_pairs = body_length < 0 ? u16{0} : static_cast<u16>(body_length / 8);

  u16 animation = EncodeLogoAnimation(palette, raw_speed);
  animation |= static_cast<u16>((packet_pairs & 0x4000) >> 14);

  u32 length_code = (((u32{packet_pairs} << 16) | 0x3f80) & 0x3f80ffff) * 2;
  const u32 sign_fill = static_cast<u32>(static_cast<s32>(static_cast<s8>(length_code >> 8)));
  length_code += (sign_fill & packet_pairs) << 16;

  const u32 mixed = ((u32{animation} & 0xff) << 16) + (length_code & 0xff0000) +
                    ((length_code >> 8) & 0xffff00);
  u32 auth_code =
      (u32{animation} << 16) | ((mixed << 8) & 0xff000000) | (length_code >> 16) | 0x80808080;
  auth_code ^= (auth_code & 0x200) != 0 ? WORD_SEDO : WORD_KAWA;
  memory.Write_U32(auth_code, result_addr + 4);

  DEBUG_LOG_FMT(DSPHLE,
                "GBA crypto at {:08x}: challenge {:08x}, length {:08x}, results to {:08x}, "
                "palette {:08x}, speed {:08x}: key {:08x}, auth {:08x}",
                address, challenge, program_length, result_addr, palette, raw_speed, session_key,
                auth_code);
}

GBAUCode::GBAUCode(DSPHLE* dsphle, u32 crc) : UCodeInterface(dsphle, crc)
{
}

void GBAUCode::Initialize()
{
  m_mail_handler.PushMail(DSP_INIT);
}

void GBAUCode::Update()
{
  // Keep poking the CPU while it has mail to read.
  if (m_mail_handler.HasPending())
    m_dsphle->GetSystem().GetDSP().GenerateDSPInterruptFromDSPEmu(INT_DSP);
}

void GBAUCode::HandleMail(u32 mail)
{
  if (m_upload_setup_in_progress)
  {
    // The real task skips the first three of the ten mails (the DRAM save); HLE has no DRAM to
    // save, so passing them on is harmless.
    PrepareBootUCode(mail);
    return;
  }

  switch (m_stage)
  {
  case Stage::AwaitingRequest:
    if (mail == REQUEST_MAIL)
    {
      INFO_LOG_FMT(DSPHLE, "GBA task: request received");
      m_stage = Stage::AwaitingAddress;
    }
    else
    {
      WARN_LOG_FMT(DSPHLE, "GBA task: expected the request mail, got {:08x}", mail);
    }
    break;

  case Stage::AwaitingAddress:
    ProcessGBACrypto(m_dsphle->GetSystem().GetMemory(), mail & 0x0FFFFFFF);
    m_mail_handler.PushMail(DSP_DONE);
    m_stage = Stage::AwaitingNextTask;
    break;

  case Stage::AwaitingNextTask:
    // This task compares the whole mail, prefix included.
    if (mail == MAIL_NEW_UCODE)
      m_upload_setup_in_progress = true;
    else if (mail == MAIL_RESET)
      m_dsphle->SetUCode(UCODE_ROM);
    else
      WARN_LOG_FMT(DSPHLE, "GBA task: unknown task mail {:08x}", mail);
    break;
  }
}

void GBAUCode::DoState(PointerWrap& p)
{
  DoStateShared(p);
  p.Do(m_stage);
}
}  // namespace DSP::HLE
