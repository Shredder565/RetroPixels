// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Real3D.cpp
//
// Memory, texture upload, DMA and frame control of the Real3D Pro-1000 board.
//
// Frame model: the board renders the scene database while the CPU builds the
// next one. Once the tile generator's ping-pong line is reached the status bit
// flips, and from then on writes to high culling RAM and polygon RAM do not go
// to the memory being drawn. The board keeps them in update lists in low
// culling RAM (each list block is: first word address, last word address, the
// words) and copies them to their destinations at the next vertical blank.
// The list for high culling RAM starts at word 0 of low culling RAM, the list
// for polygon RAM at the word given by configuration register 0.

#include "Real3D.h"

#include "Supermodel.h"
#include "JTAG.h"
#include "Util/BMPFile.h"
#include "Util/BitCast.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace
{
  // Byte offsets of the memories in m_mem.
  constexpr uint32_t kLowCull   = 0x0000000;  // 4 MB at 0x8C000000
  constexpr uint32_t kHighCull  = 0x0400000;  // 1 MB at 0x8E000000
  constexpr uint32_t kPoly      = 0x0500000;  // 4 MB at 0x98000000
  constexpr uint32_t kTexRAM    = 0x0900000;  // 8 MB: two 2048x1024 sheets of 16-bit texels
  constexpr uint32_t kFIFO      = 0x1100000;  // 1 MB texture FIFO at 0x94000000
  constexpr uint32_t kMemBytes  = 0x1200000;
  constexpr uint32_t kCopyBytes = kFIFO;      // the render thread copy holds everything but the FIFO
  constexpr uint32_t kMemWords  = kMemBytes / 4;
  constexpr uint32_t kFIFOWords = 0x100000 / 4;

  constexpr unsigned kPageShift = 12;
  constexpr uint32_t kPageBytes = 1u << kPageShift;

  constexpr uint32_t kNoBlock = 0xFFFFFFFFu;

  // Texture RAM layout of the mipmap chain inside a 2048x1024 sheet: level n
  // starts at these coordinates and is 2^n times smaller than level 0.
  constexpr unsigned kMipX[] = { 0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047 };
  constexpr unsigned kMipY[] = { 0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023, 1023 };

  // Within an uploaded tile, texels are stored in 2x2 groups, rows of groups
  // first, with the two texels of each row of a group swapped.
  inline unsigned TileTexel(unsigned col, unsigned row, unsigned tileWidth)
  {
    return (row >> 1) * (tileWidth * 2) + (row & 1) * 2 + (col >> 1) * 4 + ((col & 1) ^ 1);
  }

  inline uint32_t Swap32(uint32_t v)
  {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
  }
}

/******************************************************************************
 Memory helpers
******************************************************************************/

uint32_t *CReal3D::Words(uint32_t byteOffset) const
{
  return reinterpret_cast<uint32_t *>(m_mem.get() + byteOffset);
}

// Marks the page holding a byte as changed, for the render thread's copy.
void CReal3D::Touch(uint32_t byteOffset)
{
  if (m_threaded && byteOffset < kCopyBytes)
    m_changed[byteOffset >> kPageShift] = 1;
}

uint32_t CReal3D::CopyChangedPages(bool everything)
{
  if (everything)
  {
    std::memcpy(m_copy.get(), m_mem.get(), kCopyBytes);
    std::fill(m_changed.begin(), m_changed.end(), 0);
    return kCopyBytes;
  }
  // A changed page is copied with the first word of the page after it, unless
  // it is the last page of one of the memories.
  uint32_t copied = 0;
  for (size_t page = 0; page < m_changed.size(); page++)
  {
    if (m_changed[page])
    {
      uint32_t at = uint32_t(page << kPageShift);
      uint32_t end = at + kPageBytes;
      bool lastOfMemory = end == kHighCull || end == kPoly || end == kTexRAM || end == kCopyBytes;
      uint32_t bytes = lastOfMemory ? kPageBytes : kPageBytes + 4;
      std::memcpy(m_copy.get() + at, m_mem.get() + at, bytes);
      m_changed[page] = 0;
      copied += bytes;
    }
  }
  return copied;
}

// Appends one word to an update list. blockAt is the list's current block
// (a word offset into m_mem), regionStart where the list begins.
void CReal3D::BufferedWrite(uint32_t &blockAt, uint32_t regionStart, uint32_t wordAddr, uint32_t data)
{
  uint32_t *w = Words(0);
  auto get = [&](uint32_t i) -> uint32_t { return i < kMemWords ? w[i] : 0; };
  auto put = [&](uint32_t i, uint32_t v) { if (i < kMemWords) w[i] = v; };

  if (blockAt == kNoBlock)
  {
    blockAt = regionStart;
    put(blockAt, wordAddr);
  }
  else if (get(blockAt + 1) + 1 != wordAddr)
  {
    // Not contiguous with the current block: open a new block after it.
    blockAt += 2 + (get(blockAt + 1) - get(blockAt) + 1);
    put(blockAt, wordAddr);
  }
  put(blockAt + 1, wordAddr);
  put(blockAt + 2 + (wordAddr - get(blockAt)), data);
}

// Copies an update list to its destination (a byte offset into m_mem).
void CReal3D::ReplayBuffered(uint32_t lastBlock, uint32_t regionStart, uint32_t destOffset)
{
  if (lastBlock == kNoBlock)
    return;
  uint32_t *w = Words(0);
  uint32_t block = regionStart;
  while (block + 1 < kMemWords)
  {
    uint32_t first = w[block];
    uint32_t count = w[block + 1] - first + 1;
    for (uint32_t i = 0; i < count; i++)
    {
      uint32_t src = block + 2 + i;
      if (src >= kMemWords)
        break;
      uint64_t dst = uint64_t(destOffset) + uint64_t(first + i) * 4;
      if (dst + 4 > kMemBytes)
        continue;
      Words(uint32_t(dst))[0] = w[src];
      Touch(uint32_t(dst));
    }
    if (block == lastBlock || uint64_t(block) + 2 + count >= kMemWords)
      break;
    block += 2 + count;
  }
}

/******************************************************************************
 Save states
******************************************************************************/

void CReal3D::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("Real3D", __FILE__);

  SaveState->Write(m_mem.get(), kMemBytes);
  SaveState->Write(&m_fifoCount, sizeof(m_fifoCount));
  SaveState->Write(m_vromPort, sizeof(m_vromPort));

  SaveState->Write(&m_dmaSrc, sizeof(m_dmaSrc));
  SaveState->Write(&m_dmaDst, sizeof(m_dmaDst));
  SaveState->Write(&m_dmaLen, sizeof(m_dmaLen));
  SaveState->Write(&m_dmaResult, sizeof(m_dmaResult));
  SaveState->Write(&m_dmaReserved, sizeof(m_dmaReserved));
  SaveState->Write(&m_dmaStatus, sizeof(m_dmaStatus));
  SaveState->Write(&m_dmaCtrl, sizeof(m_dmaCtrl));

  SaveState->Write(m_renderConfig, sizeof(m_renderConfig));
  SaveState->Write(m_sceneDone);
  SaveState->Write(&m_pingPong, sizeof(m_pingPong));
  SaveState->Write(m_modeword, sizeof(m_modeword));
  SaveState->Write(&m_vromPortCount, sizeof(m_vromPortCount));
  SaveState->Write(m_cfg, sizeof(m_cfg));

  // Update list positions, relative to where each list starts (-1: empty).
  uint32_t polyAt = (m_polyBlock == kNoBlock) ? kNoBlock : m_polyBlock - m_cfg[0];
  uint32_t highAt = m_highBlock;
  SaveState->Write(&polyAt, sizeof(polyAt));
  SaveState->Write(&highAt, sizeof(highAt));
}

void CReal3D::LoadState(CBlockFile *SaveState)
{
  if (Result::OKAY != SaveState->FindBlock("Real3D"))
  {
    ErrorLog("Unable to load Real3D GPU state. Save state file is corrupt.");
    return;
  }

  SaveState->Read(m_mem.get(), kMemBytes);
  if (m_threaded)
    CopyChangedPages(true);
  m_render->UploadTextures(0, 0, 0, 2048, 2048);

  SaveState->Read(&m_fifoCount, sizeof(m_fifoCount));
  SaveState->Read(m_vromPort, sizeof(m_vromPort));

  SaveState->Read(&m_dmaSrc, sizeof(m_dmaSrc));
  SaveState->Read(&m_dmaDst, sizeof(m_dmaDst));
  SaveState->Read(&m_dmaLen, sizeof(m_dmaLen));
  SaveState->Read(&m_dmaResult, sizeof(m_dmaResult));
  SaveState->Read(&m_dmaReserved, sizeof(m_dmaReserved));
  SaveState->Read(&m_dmaStatus, sizeof(m_dmaStatus));
  SaveState->Read(&m_dmaCtrl, sizeof(m_dmaCtrl));

  SaveState->Read(m_renderConfig, sizeof(m_renderConfig));
  SaveState->Read(&m_sceneDone);
  SaveState->Read(&m_pingPong, sizeof(m_pingPong));
  SaveState->Read(m_modeword, sizeof(m_modeword));
  m_render->SetSunClamp((m_modeword[int(CASIC::Name::Mars)] & 0x40000) == 0);
  m_render->SetBlockCulling((m_modeword[int(CASIC::Name::Mercury)] & 4) != 0);
  SaveState->Read(&m_vromPortCount, sizeof(m_vromPortCount));
  SaveState->Read(m_cfg, sizeof(m_cfg));

  uint32_t polyAt = 0, highAt = 0;
  SaveState->Read(&polyAt, sizeof(polyAt));
  SaveState->Read(&highAt, sizeof(highAt));
  m_polyBlock = (polyAt == kNoBlock) ? kNoBlock : m_cfg[0] + polyAt;
  m_highBlock = highAt;
}

/******************************************************************************
 Frame control
******************************************************************************/

bool CReal3D::PingPongChanged(void) const
{
  return m_pingPong != m_pingPongAtVBlank;
}

// The board begins drawing the scene that was completed.
void CReal3D::StartFrame(void)
{
  m_frameAsked = false;
  m_sceneDone = false;
  m_noRenderForFrame = (m_modeword[int(CASIC::Name::Mercury)] & 4) != 0;
  if (!m_threaded)
    m_render->SetBlockCulling(m_noRenderForFrame);
}

void CReal3D::BeginVBlank(void)
{
  m_pingPongAtVBlank = m_pingPong;

  ReplayBuffered(m_highBlock, kLowCull / 4, kHighCull);
  ReplayBuffered(m_polyBlock, kLowCull / 4 + m_cfg[0], kPoly);
  m_polyBlock = kNoBlock;
  m_highBlock = kNoBlock;

  // A scene finished after the flip is drawn now.
  if (m_sceneDone)
    DrainTextureFIFO();
  if (m_frameAsked && m_sceneDone)
    StartFrame();
}

void CReal3D::EndVBlank(void)
{
  m_fifoErrorShown = false;
}

void CReal3D::FlipPingPongBit(void)
{
  m_pingPong = !m_pingPong;
  m_frameAsked = false;
  m_sceneDone = false;
}

void CReal3D::TilegenDrawFrame(uint32_t flags)
{
  (void)flags;
  m_frameAsked = true;
  if (!PingPongChanged() && m_sceneDone)
    StartFrame();
}

void CReal3D::Flush(void)
{
  m_sceneDone = true;
  if (!PingPongChanged())
  {
    DrainTextureFIFO();
    if (m_frameAsked)
      StartFrame();
  }
}

uint32_t CReal3D::SyncSnapshots(void)
{
  if (!m_threaded)
    return 0;
  m_uploadsForRender = m_uploads;
  m_uploads.clear();
  m_render->SetBlockCulling(m_noRenderForFrame);
  return CopyChangedPages(false);
}

void CReal3D::BeginFrame(void)
{
  if (m_threaded)
  {
    for (const PendingUpload &u : m_uploadsForRender)
      m_render->UploadTextures(u.level, u.x, u.y, u.width, u.height);
    m_uploadsForRender.clear();
  }
  m_render->BeginFrame();
}

void CReal3D::RenderFrame(void)
{
  m_render->RenderFrame();
}

void CReal3D::EndFrame(void)
{
  m_render->EndFrame();
}

/******************************************************************************
 Textures

 An upload starts with a header word:
   bits  0-5   X position in units of 32 texels
   bits  7-11  Y position in units of 32 texels
   bits 14-16  width  (32 << n)
   bits 17-19  height (32 << n)
   bit  20     sheet (0: texels 0-1023 of texture RAM, 1: rows 1024-2047)
   bit  21     8-bit data goes to the low byte of each texel
   bit  22     8-bit data goes to the high byte of each texel
   bit  23     16-bit texels (otherwise 8-bit)
   bits 24-31  0: texture and its mipmaps, 1: texture only, 2: mipmaps only
******************************************************************************/

void CReal3D::TellRenderer(unsigned level, unsigned x, unsigned y, unsigned w, unsigned h)
{
  if (m_threaded)
    m_uploads.push_back({ level, x, y, w, h });
  else
    m_render->UploadTextures(level, x, y, w, h);
}

// Writes one mipmap level. Texels arrive in tiles of up to 8x8. Returns how
// many 16-bit source words the level used.
unsigned CReal3D::WriteTexels(unsigned level, unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *src, bool wide, bool toLow, bool toHigh)
{
  const unsigned tw = std::min(8u, w);
  const unsigned th = std::min(8u, h);
  uint16_t *ram = reinterpret_cast<uint16_t *>(m_mem.get() + kTexRAM);
  const size_t ramLimit = (kMemBytes - kTexRAM) / 2;   // writes may run on into the FIFO, like the hardware's flat memory
  const uint16_t *memStart = reinterpret_cast<const uint16_t *>(m_mem.get());
  const uint16_t *memEnd = reinterpret_cast<const uint16_t *>(m_mem.get() + kMemBytes);
  const bool srcInMem = src >= memStart && src < memEnd;
  auto fetch = [&](size_t i) -> uint16_t {
    const uint16_t *p = src + i;
    if (srcInMem && p >= memEnd)
      return 0;
    return *p;
  };
  unsigned used = 0;

  if (wide)
  {
    for (unsigned ty = y; ty < y + h; ty += th)
    {
      for (unsigned tx = x; tx < x + w; tx += tw)
      {
        for (unsigned row = 0; row < th; row++)
        {
          size_t dst = size_t(ty + row) * 2048 + tx;
          for (unsigned col = 0; col < tw; col++, dst++)
          {
            if (dst < ramLimit)
            {
              ram[dst] = fetch(TileTexel(col, row, tw));
              Touch(uint32_t(kTexRAM + dst * 2));
            }
          }
        }
        src += tw * th;
        used += tw * th;
      }
    }
  }
  else
  {
    // 8-bit texels are expanded into one byte of each 16-bit texel; the
    // source pairs are read with rows and columns of each 2x2 group swapped.
    const unsigned perTile = std::max(1u, tw * th / 2);
    const uint16_t keep = toLow ? (toHigh ? 0x0000 : 0xFF00) : (toHigh ? 0x00FF : 0xFFFF);
    for (unsigned ty = y; ty < y + h; ty += th)
    {
      for (unsigned tx = x; tx < x + w; tx += tw)
      {
        if (toLow || toHigh)
        {
          for (unsigned row = 0; row < th; row++)
          {
            size_t dst = size_t(ty + row) * 2048 + tx;
            for (unsigned col = 0; col < tw; col++, dst++)
            {
              if (dst >= ramLimit)
                continue;
              unsigned pair = TileTexel(col ^ 1, row ^ 1, tw) / 2;
              uint16_t byte = (fetch(pair) >> ((col & 1) ? 0 : 8)) & 0xFF;
              uint16_t both = uint16_t(byte | (byte << 8));
              ram[dst] = uint16_t((ram[dst] & keep) | (both & ~keep));
              Touch(uint32_t(kTexRAM + dst * 2));
            }
          }
        }
        src += perTile;
        used += perTile;
      }
    }
  }

  TellRenderer(level, x, y, w, h);
  return used;
}

void CReal3D::DecodeTextureUpload(uint32_t header, const uint16_t *src)
{
  unsigned x = 32 * (header & 0x3F);
  unsigned y = 32 * ((header >> 7) & 0x1F);
  unsigned w = 32u << ((header >> 14) & 7);
  unsigned h = 32u << ((header >> 17) & 7);
  unsigned sheetY = ((header >> 20) & 1) * 1024;
  bool toLow = (header >> 21) & 1;
  bool toHigh = (header >> 22) & 1;
  bool wide = (header >> 23) & 1;
  unsigned kind = header >> 24;

  if (kind != 0 && kind != 1 && kind != 2)
    return;   // 0x80 and others: not texel data

  if (kind != 2)
    src += WriteTexels(0, x, y + sheetY, w, h, src, wide, toLow, toHigh);
  if (kind == 1)
    return;

  // Mipmaps: each level halves the size until a side would drop below 2.
  for (unsigned level = 1; w >= 4 && h >= 4 && level < 12; level++)
  {
    unsigned mx = kMipX[level] + (x >> level);
    unsigned my = kMipY[level] + (y >> level);
    w >>= 1;
    h >>= 1;
    src += WriteTexels(level, mx, my + sheetY, w, h, src, wide, toLow, toHigh);
  }
}

// Decodes every complete upload in the FIFO (word 0: size, word 1: header,
// then data), then empties it.
void CReal3D::DrainTextureFIFO(void)
{
  const uint32_t *fifo = Words(kFIFO);
  if (m_fifoCount > 2)
  {
    for (uint32_t i = 0; i < m_fifoCount - 2;)
    {
      uint32_t words = (2 + fifo[i] / 2) / 4;
      if (words == 0)
        break;
      DecodeTextureUpload(fifo[i + 1], reinterpret_cast<const uint16_t *>(&fifo[i + 2]));
      i += words;
    }
  }
  m_fifoCount = 0;
}

void CReal3D::WriteTextureFIFO(uint32_t data)
{
  if (m_fifoCount >= kFIFOWords)
  {
    if (!m_fifoErrorShown)
      ErrorLog("Overflow in Real3D texture FIFO!");
    m_fifoErrorShown = true;
    return;
  }
  Words(kFIFO)[m_fifoCount++] = data;
}

void CReal3D::WriteTexturePort(unsigned reg, uint32_t data)
{
  (void)reg;
  if (m_step == 0x10)
  {
    // Step 1.0: the word written is a VROM address of a FIFO-format upload.
    uint32_t at = data & 0xFFFFFF;
    uint32_t words = (2 + m_vrom[at] / 2) / 4;
    for (uint32_t i = 0; i < words; i++)
      WriteTextureFIFO(m_vrom[(at + i) & 0xFFFFFF]);
    return;
  }

  // Later steppings: address and header are latched; the next write uploads.
  if (m_vromPortCount < 2)
  {
    m_vromPort[m_vromPortCount++] = data;
    return;
  }
  uint32_t at = m_vromPort[0];
  uint32_t header = m_vromPort[1];
  const uint32_t *src = (at < 0x100000) ? Words(kPoly) + at : m_vrom + (at & 0xFFFFFF);  // the first 4 MB is polygon RAM
  DecodeTextureUpload(header, reinterpret_cast<const uint16_t *>(src));
  m_vromPortCount = 0;
}

/******************************************************************************
 Memory writes
******************************************************************************/

void CReal3D::WriteLowCullingRAM(uint32_t addr, uint32_t data)
{
  Touch(kLowCull + addr);
  Words(kLowCull)[addr / 4] = data;
}

void CReal3D::WriteHighCullingRAM(uint32_t addr, uint32_t data)
{
  if (PingPongChanged())
  {
    BufferedWrite(m_highBlock, kLowCull / 4, addr / 4, data);
    return;
  }
  Touch(kHighCull + addr);
  Words(kHighCull)[addr / 4] = data;
}

void CReal3D::WritePolygonRAM(uint32_t addr, uint32_t data)
{
  if (PingPongChanged())
  {
    BufferedWrite(m_polyBlock, kLowCull / 4 + m_cfg[0], addr / 4, data);
    return;
  }
  Touch(kPoly + addr);
  Words(kPoly)[addr / 4] = data;
}

void CReal3D::WriteConfigurationRegister(unsigned reg, uint32_t data)
{
  m_cfg[(reg & 0xF) / 4] = data;
}

void CReal3D::WriteJTAGModeword(CASIC::Name device, uint32_t data)
{
  if (device == CASIC::Name::Dummy)
    return;
  m_modeword[int(device)] = data;
  if (device == CASIC::Name::Mars)
    m_render->SetSunClamp((data & 0x40000) == 0);
}

/******************************************************************************
 Status registers

 Word 0: bit 25 is the ping-pong bit; the other bits read as 1 (done flags
 set, maximum clock count). Words 5-8 (byte offsets 0x14-0x20) are the line
 of sight results for priorities 0-3, as floats. The rest read as all ones.
******************************************************************************/

uint32_t CReal3D::ReadRegister(unsigned reg)
{
  if (reg == 0)
    return 0xFDFFFFFF | (m_pingPong ? 0x02000000 : 0);
  if (reg >= 0x14 && reg <= 0x20)
    return Util::FloatAsUint32(m_render->GetLosValue(int((reg - 0x14) / 4)));
  return 0xFFFFFFFF;
}

uint32_t CReal3D::ReadPCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const
{
  (void)device;
  if (bits == 8 || reg != 0)
    return 0;
  uint32_t id = Swap32(m_pciID);   // little endian device
  if (bits == 16)
    id = (id >> ((2 - offset) * 8)) & 0xFFFF;
  return id;
}

void CReal3D::WritePCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, uint32_t data)
{
  (void)device; (void)reg; (void)bits; (void)offset; (void)data;
}

/******************************************************************************
 DMA engine

 0x00 source, 0x04 destination, 0x08 length in words (writing it runs the
 copy), 0x0C status (bit 0: IRQ pending), 0x0D write 1 to acknowledge,
 0x0E control (bit 0: raise an IRQ when done, bit 7: byte swap each word),
 0x10 command, 0x14 command result.
******************************************************************************/

void CReal3D::RunDMA(void)
{
  const bool swap = (m_dmaCtrl & 0x80) != 0;
  for (; m_dmaLen != 0; m_dmaLen--)
  {
    uint32_t v = m_bus->Read32(m_dmaSrc);
    m_bus->Write32(m_dmaDst, swap ? Swap32(v) : v);
    m_dmaSrc += 4;
    m_dmaDst += 4;
  }
}

uint8_t CReal3D::ReadDMARegister8(unsigned reg) const
{
  if (reg == 0xC)
    return m_dmaStatus;
  if (reg == 0xE)
    return m_dmaCtrl;
  return 0;
}

void CReal3D::WriteDMARegister8(unsigned reg, uint8_t data)
{
  if (reg == 0xD)
  {
    if (data & 1)
    {
      m_dmaStatus &= ~1;
      m_irq->Deassert(m_dmaIRQ);
    }
  }
  else if (reg == 0xE)
    m_dmaCtrl = data;
}

uint32_t CReal3D::ReadDMARegister32(unsigned reg) const
{
  return (reg == 0x14) ? m_dmaResult : 0;
}

void CReal3D::WriteDMARegister32(unsigned reg, uint32_t data)
{
  switch (reg)
  {
  case 0x00:
    m_dmaSrc = data;
    break;
  case 0x04:
    m_dmaDst = data;
    break;
  case 0x08:
    m_dmaLen = data;
    RunDMA();
    if (m_dmaCtrl & 1)
    {
      m_dmaStatus |= 1;
      m_irq->Assert(m_dmaIRQ);
    }
    break;
  case 0x10:
    if (data & 0x20000000)
      m_dmaResult = PCIID::Step1x;            // ID query
    else if (data & 0x80000000)
      m_dmaResult = ReadRegister(data & 0x3F);  // status register read
    break;
  case 0x14:
    m_dmaResult = 0xFFFFFFFF;
    break;
  default:
    break;
  }
}

/******************************************************************************
 Set-up
******************************************************************************/

void CReal3D::Reset(void)
{
  m_fifoErrorShown = false;
  m_pingPong = 0;
  m_sceneDone = false;
  m_frameAsked = false;
  m_noRenderForFrame = false;
  m_uploads.clear();
  m_uploadsForRender.clear();
  m_fifoCount = 0;
  m_vromPortCount = 0;
  m_dmaSrc = m_dmaDst = m_dmaLen = m_dmaResult = m_dmaReserved = 0;
  m_dmaStatus = m_dmaCtrl = 0;
  std::fill(std::begin(m_cfg), std::end(m_cfg), 0);
  m_polyBlock = kNoBlock;
  m_highBlock = kNoBlock;
  std::memset(m_mem.get(), 0, kMemBytes);
  if (m_threaded)
  {
    std::memset(m_copy.get(), 0, kCopyBytes);
    std::fill(m_changed.begin(), m_changed.end(), 0);
  }
  std::fill(std::begin(m_vromPort), std::end(m_vromPort), 0);
  std::fill(std::begin(m_renderConfig), std::end(m_renderConfig), 0);
}

void CReal3D::AttachRenderer(IRender3D *Render3DPtr)
{
  m_render = Render3DPtr;
  const uint8_t *view = m_threaded ? m_copy.get() : m_mem.get();
  m_render->AttachMemory(reinterpret_cast<const uint32_t *>(view + kLowCull),
                         reinterpret_cast<const uint32_t *>(view + kHighCull),
                         reinterpret_cast<const uint32_t *>(view + kPoly),
                         m_vrom,
                         reinterpret_cast<const uint16_t *>(view + kTexRAM));
  m_render->SetStepping(m_step);
}

void CReal3D::SetStepping(int stepping)
{
  m_step = (stepping == 0x10 || stepping == 0x15 || stepping == 0x20 || stepping == 0x21) ? stepping : 0x10;
  m_pciID = (stepping >= 0x20) ? PCIID::Step2x : PCIID::Step1x;
  if (m_render)
    m_render->SetStepping(m_step);
}

Result CReal3D::Init(const uint8_t *vromPtr, IBus *BusObjectPtr, CIRQ *IRQObjectPtr, unsigned dmaIRQBit)
{
  m_bus = BusObjectPtr;
  m_irq = IRQObjectPtr;
  m_dmaIRQ = dmaIRQBit;
  m_vrom = reinterpret_cast<const uint32_t *>(vromPtr);

  m_mem.reset(new(std::nothrow) uint8_t[kMemBytes]);
  if (m_threaded)
  {
    m_copy.reset(new(std::nothrow) uint8_t[kCopyBytes]);
    m_changed.assign(kCopyBytes >> kPageShift, 0);
  }
  if (!m_mem || (m_threaded && !m_copy))
  {
    float mb = float(kMemBytes + (m_threaded ? kCopyBytes : 0)) / float(0x100000);
    return ErrorLog("Insufficient memory for Real3D object (needs %1.1f MB).", mb);
  }
  std::memset(m_mem.get(), 0, kMemBytes);
  if (m_threaded)
    std::memset(m_copy.get(), 0, kCopyBytes);
  return Result::OKAY;
}

CReal3D::CReal3D(const Util::Config::Node &config)
  : m_config(config),
    m_threaded(config["GPUMultiThreaded"].ValueAs<bool>()),
    m_polyBlock(kNoBlock),
    m_highBlock(kNoBlock)
{
}

CReal3D::~CReal3D(void)
{
  // Optional dump of texture RAM, decoded in each texel format.
  if (m_mem && m_config["DumpTextures"].ValueAsDefault<bool>(false))
  {
    const uint8_t *tex = m_mem.get() + kTexRAM;
    struct Dump { bool (*write)(const std::string &, const uint8_t *, int32_t, int32_t, bool); const char *file; const char *what; };
    const Dump dumps[] =
    {
      { Util::WriteSurfaceToBMP<Util::T1RGB5ContourEnabled>, "textures_t1rgb5_contour.bmp", "T1RGB5 (contour bit enabled)" },
      { Util::WriteSurfaceToBMP<Util::T1RGB5ContourIgnored>, "textures_t1rgb5_opaque.bmp", "T1RGB5 (contour bit ignored)" },
      { Util::WriteSurfaceToBMP<Util::A4L4Low>, "textures_a4l4_lo.bmp", "A4L4 (low)" },
      { Util::WriteSurfaceToBMP<Util::L4A4Low>, "textures_l4a4_lo.bmp", "L4A4 (low)" },
      { Util::WriteSurfaceToBMP<Util::A4L4High>, "textures_a4l4_hi.bmp", "A4L4 (high)" },
      { Util::WriteSurfaceToBMP<Util::L4A4High>, "textures_l4a4_hi.bmp", "L4A4 (high)" },
      { Util::WriteSurfaceToBMP<Util::L8Low>, "textures_l8_lo.bmp", "L8 (low)" },
      { Util::WriteSurfaceToBMP<Util::L8High>, "textures_l8_hi.bmp", "L8 (high)" },
      { Util::WriteSurfaceToBMP<Util::RGBA4>, "textures_rgba4.bmp", "RGBA4" },
      { Util::WriteSurfaceToBMP<Util::L4Channel0>, "textures_l4_0.bmp", "L4 (channel 0)" },
      { Util::WriteSurfaceToBMP<Util::L4Channel1>, "textures_l4_1.bmp", "L4 (channel 1)" },
      { Util::WriteSurfaceToBMP<Util::L4Channel2>, "textures_l4_2.bmp", "L4 (channel 2)" },
      { Util::WriteSurfaceToBMP<Util::L4Channel3>, "textures_l4_3.bmp", "L4 (channel 3)" },
    };
    for (const Dump &d : dumps)
    {
      d.write(d.file, tex, 2048, 2048, false);
      printf("Wrote textures as %s to '%s'\n", d.what, d.file);
    }
  }
  m_render = nullptr;
}
