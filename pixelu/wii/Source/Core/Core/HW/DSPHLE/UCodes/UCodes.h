// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace DSP::HLE
{
class CMailHandler;
class DSPHLE;

// Pseudo-CRCs for the ucodes that are not loaded by the game: the boot ROM and the small init
// program run when the CPU clears the DSP init bit. UCODE_NULL stands for "no ucode".
constexpr u32 UCODE_ROM = 0x00000000;
constexpr u32 UCODE_INIT_AUDIO_SYSTEM = 0x00000001;
constexpr u32 UCODE_NULL = 0xFFFFFFFF;

// Base class of every high-level emulated DSP program. The CRC identifying a ucode is the
// "ector" hash of its IRAM image.
class UCodeInterface
{
public:
  UCodeInterface(DSPHLE* dsphle, u32 crc);
  virtual ~UCodeInterface();

  virtual void Initialize() = 0;
  virtual void HandleMail(u32 mail) = 0;
  virtual void Update() = 0;
  virtual void DoState(PointerWrap& p) = 0;

  static u32 GetCRC(UCodeInterface* ucode) { return ucode != nullptr ? ucode->m_crc : UCODE_NULL; }

protected:
  // Task switching (the SDK's DSP task manager, also used by libogc).
  //
  // Mails from the DSP to the CPU carry 0xDCD1 in the high half:
  //   DSP_INIT       task started (callback, no reply)
  //   DSP_RESUME     task resumed after a switch (callback, no reply)
  //   DSP_YIELD      task wants to give up the DSP; AX expects MAIL_NEW_UCODE when the CPU has
  //                  another task, MAIL_CONTINUE otherwise
  //   DSP_DONE       task finished (callback); the CPU answers MAIL_NEW_UCODE or MAIL_RESET
  //   DSP_SYNC       generic acknowledgement (callback, no reply)
  //   DSP_FRAME_END  Zelda-family ucodes: a frame of audio is ready; the CPU answers
  //                  MAIL_NEW_UCODE when it has a task to run, MAIL_CONTINUE otherwise
  // The CPU answers with 0xCDD1 in the high half:
  //   MAIL_RESUME    AX replies DSP_RESUME and continues; Zelda halts (not seen in practice)
  //   MAIL_NEW_UCODE the next ten mails describe the ucode to boot (see PrepareBootUCode)
  //   MAIL_RESET     reboot into the ROM
  //   MAIL_CONTINUE  go back to the main loop
  static constexpr u32 TASK_MAIL_MASK = 0xFFFF0000;
  static constexpr u32 TASK_MAIL_TO_CPU = 0xDCD10000;
  static constexpr u32 DSP_INIT = TASK_MAIL_TO_CPU | 0x0000;
  static constexpr u32 DSP_RESUME = TASK_MAIL_TO_CPU | 0x0001;
  static constexpr u32 DSP_YIELD = TASK_MAIL_TO_CPU | 0x0002;
  static constexpr u32 DSP_DONE = TASK_MAIL_TO_CPU | 0x0003;
  static constexpr u32 DSP_SYNC = TASK_MAIL_TO_CPU | 0x0004;
  static constexpr u32 DSP_FRAME_END = TASK_MAIL_TO_CPU | 0x0005;

  static constexpr u32 TASK_MAIL_TO_DSP = 0xCDD10000;
  static constexpr u32 MAIL_RESUME = TASK_MAIL_TO_DSP | 0x0000;
  static constexpr u32 MAIL_NEW_UCODE = TASK_MAIL_TO_DSP | 0x0001;
  static constexpr u32 MAIL_RESET = TASK_MAIL_TO_DSP | 0x0002;
  static constexpr u32 MAIL_CONTINUE = TASK_MAIL_TO_DSP | 0x0003;

  // Feeds one of the ten mails that follow MAIL_NEW_UCODE. After the tenth, the new ucode is
  // identified and swapped in (which may destroy this object), and the next Update() of the
  // resumed ucode is told to send its resume mail (see NeedsResumeMail).
  void PrepareBootUCode(u32 mail);

  // True once after a ucode switch requested through PrepareBootUCode completed. Ucodes that
  // can be resumed (AX, Zelda) send DSP_RESUME when they see it.
  bool NeedsResumeMail();

  // Saves the task-switch state shared by all ucodes.
  void DoStateShared(PointerWrap& p);

  CMailHandler& m_mail_handler;

  // Set by a ucode when it receives MAIL_NEW_UCODE; mails then go to PrepareBootUCode until the
  // description is complete (which clears it again).
  bool m_upload_setup_in_progress = false;

  DSPHLE* m_dsphle;

  // The ucode's CRC, used to rebuild the ucode when loading a state and to tell versions apart.
  u32 m_crc;

private:
  // The ten-mail description of the next task. Saved as raw bytes; `reserved` makes the
  // alignment padding explicit so the saved bytes are always defined.
  struct BootRequest
  {
    u32 mram_dest_addr;
    u16 mram_size;
    u16 mram_dram_addr;
    u32 iram_mram_addr;
    u16 iram_size;
    u16 iram_dest;
    u16 iram_startpc;
    u16 reserved;
    u32 dram_mram_addr;
    u16 dram_size;
    u16 dram_dest;
  };
  static_assert(sizeof(BootRequest) == 28);

  BootRequest m_boot_request{};
  int m_boot_mails_received = 0;
  bool m_needs_resume_mail = false;
};

// Hashes the ucode image at `address` in main memory (dumping it when the user asked for ucode
// dumps) and returns its CRC.
u32 IdentifyUCodeImage(DSPHLE* dsphle, u32 address, u32 size);

// Creates the HLE implementation for a CRC. Unknown CRCs fall back to AX (GameCube) or AXWii
// (Wii) after warning the user; UCODE_NULL gives nullptr.
std::unique_ptr<UCodeInterface> UCodeFactory(u32 crc, DSPHLE* dsphle, bool wii);
}  // namespace DSP::HLE
