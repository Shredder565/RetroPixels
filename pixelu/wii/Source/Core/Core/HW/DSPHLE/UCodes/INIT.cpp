// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/UCodes/INIT.h"

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"

namespace DSP::HLE
{
INITUCode::INITUCode(DSPHLE* dsphle, u32 crc) : UCodeInterface(dsphle, crc)
{
  INFO_LOG_FMT(DSPHLE, "DSP init program started");
}

void INITUCode::Initialize()
{
  // "\x80TCH"
  m_mail_handler.PushMail(0x80544348);
}

void INITUCode::HandleMail(u32 /*mail*/)
{
}

void INITUCode::Update()
{
}

void INITUCode::DoState(PointerWrap& /*p*/)
{
  // Nothing to save: this program cannot start a task, so it has no task-switch state either.
}
}  // namespace DSP::HLE
