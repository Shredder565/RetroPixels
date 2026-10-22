// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "Core/Boot/Boot.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"
#include "DiscIO/VolumeWad.h"

// Starts a title that is installed on the emulated NAND, the way the system
// menu would launch a channel.
bool CBoot::BootNANDTitle(Core::System& system, const u64 title_id)
{
  // Mark the boot as a NAND boot (TYPE_NANDBOOT) in the system menu state.
  constexpr u8 STATE_TYPE_NANDBOOT = 0x04;
  UpdateStateFlags([](StateFlags* state) { state->type = STATE_TYPE_NANDBOOT; });

  auto es = system.GetIOS()->GetESDevice();

  // The ticket decides which key set the title's content uses. Without one,
  // assume a retail console.
  IOS::HLE::IOSC::ConsoleType console = IOS::HLE::IOSC::ConsoleType::Retail;
  const IOS::ES::TicketReader ticket = es->GetCore().FindSignedTicket(title_id);
  if (!ticket.IsValid())
    ERROR_LOG_FMT(BOOT, "No ticket was found for {:016x}", title_id);
  else
    console = ticket.GetConsoleType();

  SetupWiiMemory(system, console);
  return es->LaunchTitle(title_id);
}

// A WAD is booted by installing it temporarily and then launching it as a
// NAND title.
bool CBoot::Boot_WiiWAD(Core::System& system, const DiscIO::VolumeWAD& wad)
{
  const bool installed =
      WiiUtils::InstallWAD(*system.GetIOS(), wad, WiiUtils::InstallType::Temporary);
  if (!installed)
  {
    PanicAlertFmtT("Cannot boot this WAD because it could not be installed to the NAND.");
    return false;
  }
  return BootNANDTitle(system, wad.GetTMD().GetTitleId());
}
