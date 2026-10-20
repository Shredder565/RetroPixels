// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "Core/HW/EXI/EXI_Device.h"

class MemoryCardBase;
class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
class CoreTimingManager;
}
namespace Memcard
{
struct HeaderData;
}
namespace Movie
{
class MovieManager;
}

namespace ExpansionInterface
{
enum class Slot : int;

enum class AllowMovieFolder
{
  Yes,
  No,
};

// A GameCube memory card on slot A or B: the card's flash command set over EXI, backed by a raw
// card image or a folder of GCI files.
class CEXIMemoryCard : public IEXIDevice
{
public:
  CEXIMemoryCard(Core::System& system, Slot slot, bool gci_folder,
                 const Memcard::HeaderData& header_data);
  ~CEXIMemoryCard() override;
  void SetCS(int cs) override;
  bool IsInterruptSet() override;
  bool UseDelayedTransferCompletion() const override;
  bool IsPresent() const override;
  void DoState(PointerWrap& p) override;
  void DMARead(u32 addr, u32 size) override;
  void DMAWrite(u32 addr, u32 size) override;

  // Registers the per-slot CoreTiming events. Has to run at boot: CoreTiming's savestate is
  // loaded before the EXI devices exist, and pending card events must find their callbacks.
  static void Init(CoreTiming::CoreTimingManager& core_timing);
  static void Shutdown();

  static std::pair<std::string /* path */, bool /* migrate */>
  GetGCIFolderPath(Slot card_slot, AllowMovieFolder allow_movie_folder, Movie::MovieManager& movie);

protected:
  void TransferByte(u8& byte) override;

private:
  void OpenFolderCard(const Memcard::HeaderData& header_data);
  void OpenImageCard(u16 size_mbits);

  static void OnCommandDone(Core::System& system, u64 userdata, s64 cycles_late);
  static void OnTransferDone(Core::System& system, u64 userdata, s64 cycles_late);
  static CEXIMemoryCard* FindCard(Core::System& system, u64 userdata);

  void FinishCommand();
  void FinishCommandAfter(u64 cycles);
  void ScheduleTransferDone(u32 size, u32 bytes_per_second);

  void FinishErase();
  void FinishProgram();

  u32 ClampedAddress() const;

  Slot m_card_slot;

  // Card state, saved in this order (only while a movie is active, see DoState).
  int m_irq_enabled = 0;
  bool m_irq_pending = false;
  // The current command byte, kept as an int because savestates store it in 4 bytes.
  int m_opcode = 0;
  int m_status = 0;
  u32 m_position = 0;
  std::array<u8, 128> m_page_buffer{};
  u32 m_address = 0;

  // The two bytes the card returns for the "read ID" command.
  u16 m_chip_id = 0;
  // Capacity in bytes; a power of two, used to wrap addresses.
  u32 m_capacity = 0;
  std::unique_ptr<MemoryCardBase> m_backing;
};
}  // namespace ExpansionInterface
