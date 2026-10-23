// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// TileGen.h
//
// The Model 3 tile generator: 1 MB of pattern and name table RAM, 128 KB of
// palette RAM (32768 colours, one per 32-bit word), and four scrolling tile
// layers drawn in two pairs. Each pair has a primary and an alternate layer;
// a per-line mask picks which one shows in each 32-pixel column. Lines are
// drawn into two buffers, one under and one over the 3D scene, which the 2D
// renderer composites.

#ifndef INCLUDED_TILEGEN_H
#define INCLUDED_TILEGEN_H

#include "IRQ.h"
#include "Graphics/Render2D.h"
#include "TileGenBuffer.h"

#include <cstdint>
#include <memory>

class CTileGen
{
public:
  // Save states: block "Tile Generator".
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  void BeginVBlank(void);
  void EndVBlank(void);
  UINT32 SyncSnapshots(void);   // hands the finished layer buffers to the renderer
  void BeginFrame(void);
  void PreRenderFrame(void);
  void RenderFrameBottom(void);
  void RenderFrameTop(void);
  void EndFrame(void);

  // RAM at 0xF1000000-0xF111FFFF, little endian. addr < 0x120000.
  uint8_t ReadRAM8(unsigned addr) const;
  uint16_t ReadRAM16(unsigned addr) const;
  uint32_t ReadRAM32(unsigned addr) const;
  void WriteRAM8(unsigned addr, uint8_t data);
  void WriteRAM16(unsigned addr, uint16_t data);
  void WriteRAM32(unsigned addr, uint32_t data);

  // Registers at 0xF1180000-0xF11800FF.
  UINT32 ReadRegister(unsigned reg) const;
  void WriteRegister(unsigned reg, UINT32 data);

  void Reset(void);
  void AttachRenderer(CRender2D *Render2DPtr);
  Result Init(CIRQ *IRQObjectPtr);

  // Draws one line (0-383) of all enabled layers.
  void DrawLine(int line);

  CTileGen(const Util::Config::Node &config);
  ~CTileGen(void);

private:
  struct ColourAdjust
  {
    int r = 0, g = 0, b = 0;
  };

  static ColourAdjust DecodeAdjust(uint32_t reg);
  uint32_t ToABGR(const ColourAdjust &adjust, uint32_t colour) const;
  void SetColour(unsigned index, uint32_t colour);
  void RebuildColours(int pair);
  uint32_t Word(unsigned index) const;

  CIRQ *m_irq = nullptr;
  CRender2D *m_render = nullptr;

  std::unique_ptr<uint8_t[]> m_ram;
  UINT32 m_regs[64];

  // Palette RAM decoded to ABGR, one table per layer pair (each pair has its
  // own colour offset register).
  ColourAdjust m_adjust[2];
  std::unique_ptr<uint32_t[]> m_colours[2];

  // Index 0: under the 3D scene, 1: over it.
  std::shared_ptr<TileGenBuffer> m_drawing[2];
  std::shared_ptr<TileGenBuffer> m_finished[2];
};

#endif  // INCLUDED_TILEGEN_H
