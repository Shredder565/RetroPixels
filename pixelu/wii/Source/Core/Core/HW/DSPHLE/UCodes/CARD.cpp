// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/UCodes/CARD.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"
#include "Core/System.h"

namespace DSP::HLE
{
namespace
{
constexpr u32 MAIL_UNLOCK_CARD = 0xFF000000;
}

CARDUCode::CARDUCode(DSPHLE* dsphle, u32 crc) : UCodeInterface(dsphle, crc)
{
  INFO_LOG_FMT(DSPHLE, "Memory card unlock task started");
}

void CARDUCode::Initialize()
{
  m_mail_handler.PushMail(DSP_INIT);
}

void CARDUCode::Update()
{
  // Keep poking the CPU while it has mail to read.
  if (m_mail_handler.HasPending())
    m_dsphle->GetSystem().GetDSP().GenerateDSPInterruptFromDSPEmu(INT_DSP);
}

void CARDUCode::HandleMail(u32 mail)
{
  if (mail != MAIL_UNLOCK_CARD)
    WARN_LOG_FMT(DSPHLE, "Memory card task: unexpected mail {:08x}", mail);

  // Report the task as done and go back to the ROM. Rebooting the ROM clears the queue, so the
  // CPU actually sees the ROM's greeting.
  m_mail_handler.PushMail(DSP_DONE);
  m_dsphle->SetUCode(UCODE_ROM);
}

void CARDUCode::DoState(PointerWrap& p)
{
  DoStateShared(p);
  p.Do(m_stage);
}
}  // namespace DSP::HLE
