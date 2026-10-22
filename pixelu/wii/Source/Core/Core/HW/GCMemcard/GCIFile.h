// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/HW/GCMemcard/GCMemcard.h"

class PointerWrap;

namespace Memcard
{
// One save as held by the GCI-folder memory card: its directory entry, its
// data blocks (loaded lazily from disk) and the card blocks it occupies.
class GCIFile
{
public:
  // Reads the 0x40-byte directory entry at the start of m_filename.
  bool LoadHeader();
  // Reads the data blocks if they are not in memory yet.
  bool LoadSaveBlocks();
  // True for the saves whose games tie their contents to the card's position
  // (see PSO_MakeSaveGameValid and FZEROGX_MakeSaveGameValid).
  bool HasCopyProtection() const;
  void DoState(PointerWrap& p);
  // Position of a card block within this save, or -1 if it is not part of it.
  int UsesBlock(u16 blocknum);

  DEntry m_gci_header;
  std::vector<GCMBlock> m_save_data;
  std::vector<u16> m_used_blocks;
  bool m_dirty = false;
  std::string m_filename;
};
}  // namespace Memcard
