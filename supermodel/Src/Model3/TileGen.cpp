// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// TileGen.cpp
//
// RAM map (byte offsets):
//   0x000000-0x0F5FFF  patterns (4-bit tiles: 8 words each, 8-bit: 16 words)
//   0x0F6000-0x0F6FFF  line scroll tables, 0x400 bytes per layer, two lines
//                      per word (the even line in the upper half)
//   0x0F7000-0x0F77FF  line masks, one word per line: upper half for layers
//                      0/1, lower half for layers 2/3; bit 15 is the leftmost
//                      32-pixel column, a set bit shows the primary layer
//   0x0F8000-0x0FFFFF  name tables, 0x2000 bytes per layer: 64 rows of 32
//                      words, each word two 8x8 tile entries (upper half the
//                      left tile): pattern number, palette bank in the upper
//                      bits
//   0x100000-0x11FFFF  palette, one word per colour (low 16 bits: xBGR 1555,
//                      bit 15 set means transparent)
//
// Registers (word index = byte offset / 4):
//   0x10 write: acknowledge IRQs (bits 0-7)
//   0x20 bits 8-11: layer drawn over the 3D scene; bits 12-15: 4-bit layer
//   0x40 colour offset for layers 0/1, 0x44 for layers 2/3 (signed bytes,
//        red in bits 0-7, green 8-15, blue 16-23, applied doubled)
//   0x60-0x6C one per layer: bit 31 enable, bits 16-24 Y scroll, bit 15 line
//        scroll, bits 0-9 X scroll

#include "TileGen.h"
#include "Supermodel.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace
{
  constexpr unsigned kRamBytes = 0x120000;
  constexpr unsigned kPaletteStart = 0x100000;
  constexpr unsigned kColours = 0x8000;

  constexpr unsigned kLineScrollWord = 0xF6000 / 4;
  constexpr unsigned kLineMaskWord = 0xF7000 / 4;
  constexpr unsigned kNameTableWord = 0xF8000 / 4;

  constexpr int kVisibleWidth = 496;

  inline int Clamp255(int v)
  {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
  }
}

/******************************************************************************
 Colours
******************************************************************************/

CTileGen::ColourAdjust CTileGen::DecodeAdjust(uint32_t reg)
{
  ColourAdjust a;
  a.r = int(int8_t(reg & 0xFF)) * 2;
  a.g = int(int8_t((reg >> 8) & 0xFF)) * 2;
  a.b = int(int8_t((reg >> 16) & 0xFF)) * 2;
  return a;
}

uint32_t CTileGen::ToABGR(const ColourAdjust &adjust, uint32_t colour) const
{
  const int alpha = (colour & 0x8000) ? 0 : 255;
  auto expand = [&](unsigned shift) { return int(((colour >> shift) & 0x1F) * 255 / 31) & alpha; };
  int r = Clamp255(expand(0) + adjust.r);
  int g = Clamp255(expand(5) + adjust.g);
  int b = Clamp255(expand(10) + adjust.b);
  return (uint32_t(alpha) << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
}

void CTileGen::SetColour(unsigned index, uint32_t colour)
{
  m_colours[0][index] = ToABGR(m_adjust[0], colour);
  m_colours[1][index] = ToABGR(m_adjust[1], colour);
}

void CTileGen::RebuildColours(int pair)
{
  for (unsigned i = 0; i < kColours; i++)
    m_colours[pair][i] = ToABGR(m_adjust[pair], Word(kPaletteStart / 4 + i));
}

uint32_t CTileGen::Word(unsigned index) const
{
  uint32_t v;
  std::memcpy(&v, &m_ram[index * 4], 4);
  return v;
}

/******************************************************************************
 Save states
******************************************************************************/

void CTileGen::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("Tile Generator", __FILE__);
  SaveState->Write(m_ram.get(), kRamBytes);
  SaveState->Write(m_regs, sizeof(m_regs));
}

void CTileGen::LoadState(CBlockFile *SaveState)
{
  if (Result::OKAY != SaveState->FindBlock("Tile Generator"))
  {
    ErrorLog("Unable to load tile generator state. Save state file is corrupt.");
    return;
  }
  SaveState->Read(m_ram.get(), kRamBytes);
  SaveState->Read(m_regs, sizeof(m_regs));

  m_adjust[0] = DecodeAdjust(m_regs[0x40 / 4]);
  m_adjust[1] = DecodeAdjust(m_regs[0x44 / 4]);
  RebuildColours(0);
  RebuildColours(1);

  // Redraw the whole screen so the restored frame shows at once.
  for (auto &buffer : m_drawing)
    buffer->Clear();
  for (int line = 0; line < TileGenBuffer::Height; line++)
    DrawLine(line);
  SyncSnapshots();
}

/******************************************************************************
 Frame
******************************************************************************/

void CTileGen::BeginVBlank(void)
{
  for (auto &buffer : m_drawing)
    buffer->Clear();
}

void CTileGen::EndVBlank(void)
{
}

UINT32 CTileGen::SyncSnapshots(void)
{
  std::swap(m_drawing[0], m_finished[0]);
  std::swap(m_drawing[1], m_finished[1]);
  m_render->AttachDrawBuffers(m_finished[0], m_finished[1]);
  return 0;
}

void CTileGen::BeginFrame(void)
{
  m_render->BeginFrame();
}

void CTileGen::PreRenderFrame(void)
{
  m_render->PreRenderFrame();
}

void CTileGen::RenderFrameBottom(void)
{
  m_render->RenderFrameBottom();
}

void CTileGen::RenderFrameTop(void)
{
  m_render->RenderFrameTop();
}

void CTileGen::EndFrame(void)
{
  m_render->EndFrame();
}

/******************************************************************************
 Line drawing
******************************************************************************/

void CTileGen::DrawLine(int line)
{
  // Layers 2/3 first, then 0/1, so the first pair can cover the second where
  // both go to the same buffer.
  for (int pair = 1; pair >= 0; pair--)
  {
    struct Layer
    {
      bool on;
      UINT32 *out;
      int scrollX, scrollY;
      bool fourBit;
      unsigned names;
    } layers[2];

    for (int k = 0; k < 2; k++)
    {
      const int n = pair * 2 + k;
      const uint32_t ctrl = m_regs[0x60 / 4 + n];
      Layer &L = layers[k];
      L.on = (ctrl & 0x80000000) != 0;
      L.out = m_drawing[(m_regs[0x20 / 4] >> (8 + n)) & 1]->GetLine(line);
      if (ctrl & 0x8000)
        L.scrollX = int((Word(kLineScrollWord + n * 0x100 + line / 2) >> ((line & 1) ? 0 : 16)) & 0xFFFF);
      else
        L.scrollX = int(ctrl & 0x3FF);
      L.scrollY = int((ctrl >> 16) & 0x1FF);
      L.fourBit = ((m_regs[0x20 / 4] >> (12 + n)) & 1) != 0;
      L.names = kNameTableWord + n * 0x800;
    }
    if (!layers[0].on && !layers[1].on)
      continue;

    const unsigned mask = (Word(kLineMaskWord + line) >> (pair == 0 ? 16 : 0)) & 0xFFFF;
    auto primaryAt = [mask](int x) { return ((mask >> (15 - x / 32)) & 1) != 0; };
    const uint32_t *colours = m_colours[pair].get();

    int x = 0;
    while (x < kVisibleWidth)
    {
      const bool primary = primaryAt(x);
      const Layer &L = layers[primary ? 0 : 1];
      if (!L.on)
      {
        x += 32 - (x & 15);   // to the next column boundary or tile
        continue;
      }

      // A tile pair that straddles a mask column boundary is drawn only up
      // to the boundary, using the tile pair to the left.
      const bool crossesColumn = primaryAt(x + 15) != primary;
      const int column = crossesColumn ? x - 16 : x;
      const int row = line + L.scrollY;
      const unsigned entry = unsigned(((row / 8) & 63) * 32 + ((column + L.scrollX) / 16 & 31));
      const uint32_t pairData = Word(L.names + entry);
      const int fine = row & 7;
      const int from = (x + L.scrollX) & 15;
      const int to = crossesColumn ? 16 - (x & 15) : 16;

      for (int px = from; px < to; px++, x++)
      {
        const uint32_t tile = (px < 8) ? (pairData >> 16) : (pairData & 0xFFFF);
        uint32_t index;
        if (L.fourBit)
        {
          const unsigned pattern = (((tile & 0x3FFF) << 1) | ((tile >> 15) & 1)) * 8;
          const uint32_t bits = Word(pattern + fine);
          index = (tile & 0x7FF0) | ((bits >> (28 - 4 * (px & 7))) & 0xF);
        }
        else
        {
          const unsigned pattern = (tile & 0x3FFF) * 16;
          const uint32_t bits = Word(pattern + fine * 2 + ((px & 7) >> 2));
          index = (tile & 0x7F00) | ((bits >> (24 - 8 * (px & 3))) & 0xFF);
        }
        const uint32_t c = colours[index];
        if (c >= 0x1000000)     // alpha 0: transparent
          L.out[x] = c;
      }
    }
  }
}

/******************************************************************************
 RAM and registers
******************************************************************************/

uint8_t CTileGen::ReadRAM8(unsigned addr) const
{
  return m_ram[addr];
}

uint16_t CTileGen::ReadRAM16(unsigned addr) const
{
  uint16_t v;
  std::memcpy(&v, &m_ram[addr], 2);
  return v;
}

uint32_t CTileGen::ReadRAM32(unsigned addr) const
{
  uint32_t v;
  std::memcpy(&v, &m_ram[addr], 4);
  return v;
}

void CTileGen::WriteRAM32(unsigned addr, uint32_t data)
{
  std::memcpy(&m_ram[addr], &data, 4);
  if (addr >= kPaletteStart)
    SetColour((addr - kPaletteStart) / 4, data);
}

// Narrow writes are done as a read-modify-write of a 32-bit word. For 16-bit
// writes that word starts at the even address, so the palette entry is
// refreshed from the word starting there.
void CTileGen::WriteRAM16(unsigned addr, uint16_t data)
{
  const unsigned at = addr & ~1u;
  const unsigned shift = (addr & 1) * 16;
  uint32_t v = ReadRAM32(at);
  v = (v & ~(0xFFFFu << shift)) | (uint32_t(data) << shift);
  WriteRAM32(at, v);
}

void CTileGen::WriteRAM8(unsigned addr, uint8_t data)
{
  const unsigned at = addr & ~3u;
  const unsigned shift = (addr & 3) * 8;
  uint32_t v = ReadRAM32(at);
  v = (v & ~(0xFFu << shift)) | (uint32_t(data) << shift);
  WriteRAM32(at, v);
}

UINT32 CTileGen::ReadRegister(unsigned reg) const
{
  return m_regs[(reg & 0xFF) / 4];
}

void CTileGen::WriteRegister(unsigned reg, UINT32 data)
{
  reg &= 0xFF;
  UINT32 &r = m_regs[reg / 4];
  if (reg == 0x10)
    m_irq->Deassert(data & 0xFF);
  else if ((reg == 0x40 || reg == 0x44) && r != data)
  {
    const int pair = (reg == 0x40) ? 0 : 1;
    m_adjust[pair] = DecodeAdjust(data);
    r = data;
    RebuildColours(pair);
  }
  r = data;
}

/******************************************************************************
 Set-up
******************************************************************************/

// Clears RAM and registers. The decoded colours and offsets are left as they
// are until palette RAM or an offset register is written.
void CTileGen::Reset(void)
{
  std::memset(m_ram.get(), 0, kRamBytes + 4);
  std::memset(m_regs, 0, sizeof(m_regs));
}

void CTileGen::AttachRenderer(CRender2D *Render2DPtr)
{
  m_render = Render2DPtr;
  m_render->AttachVRAM(m_ram.get());
  m_render->AttachRegisters(m_regs);
}

Result CTileGen::Init(CIRQ *IRQObjectPtr)
{
  // A few spare bytes past the end let unaligned accesses at the very end of
  // palette RAM stay inside the allocation.
  m_ram.reset(new(std::nothrow) uint8_t[kRamBytes + 4]);
  if (!m_ram)
    return ErrorLog("Insufficient memory for tile generator object (needs %1.1f MB).", double(kRamBytes) / double(0x100000));
  std::memset(m_ram.get(), 0, kRamBytes + 4);
  m_irq = IRQObjectPtr;
  return Result::OKAY;
}

CTileGen::CTileGen(const Util::Config::Node &config)
  : m_regs{}
{
  (void)config;
  for (int i = 0; i < 2; i++)
  {
    m_drawing[i] = std::make_shared<TileGenBuffer>();
    m_finished[i] = std::make_shared<TileGenBuffer>();
    m_colours[i].reset(new uint32_t[kColours]());
  }
}

CTileGen::~CTileGen(void)
{
}
