// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Real3D.h
//
// The Real3D Pro-1000 graphics board of the Sega Model 3, as the PowerPC sees
// it: the culling RAMs, polygon RAM, texture RAM and its upload FIFO, the VROM
// texture port, the board's DMA engine and status registers. The scene itself
// is drawn by an IRender3D engine, which reads the memories attached here.

#ifndef INCLUDED_REAL3D_H
#define INCLUDED_REAL3D_H

#include "IRQ.h"
#include "JTAG.h"
#include "PCI.h"
#include "CPU/Bus.h"
#include "Graphics/IRender3D.h"
#include "Util/NewConfig.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class CReal3D: public IPCIDevice
{
public:
  // PCI vendor/device IDs (vendor 0x11DB is Sega). Step 1.x boards answer
  // with Mercury's ID, Step 2.x boards with the DMA ASIC's ID in PCI config
  // space. The DMA "ID" command always answers with Mercury's.
  enum PCIID: uint32_t
  {
    Step1x = 0x16C311DB,
    Step2x = 0x178611DB
  };

  // Save states: block "Real3D".
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  // Frame timing, driven by the Model 3 frame loop.
  void BeginVBlank(void);       // start of vertical blank: buffered writes land
  void EndVBlank(void);
  void FlipPingPongBit(void);   // the tile generator's ping-pong line was reached
  uint32_t SyncSnapshots(void); // multi-threaded rendering: copy changed memory for the render thread
  void BeginFrame(void);
  void RenderFrame(void);
  void EndFrame(void);

  // 0x88000000: the CPU says the scene database is complete.
  void Flush(void);

  // DMA engine registers (0xC2000000). 32-bit values are little endian.
  uint8_t ReadDMARegister8(unsigned reg) const;
  uint32_t ReadDMARegister32(unsigned reg) const;
  void WriteDMARegister8(unsigned reg, uint8_t data);
  void WriteDMARegister32(unsigned reg, uint32_t data);

  // 0x9C000000 configuration registers (ping-pong buffer sizes).
  void WriteConfigurationRegister(unsigned reg, uint32_t data);

  // Memory writes. Addresses are byte offsets, already masked by the caller;
  // data is little endian.
  void WriteLowCullingRAM(uint32_t addr, uint32_t data);    // 0x8C000000, 4 MB
  void WriteHighCullingRAM(uint32_t addr, uint32_t data);   // 0x8E000000, 1 MB
  void WriteTextureFIFO(uint32_t data);                     // 0x94000000, 1 MB FIFO
  void WriteTexturePort(unsigned reg, uint32_t data);       // 0x90000000, VROM textures
  void WritePolygonRAM(uint32_t addr, uint32_t data);       // 0x98000000, 4 MB

  // Mode words written through JTAG to the Real3D ASICs.
  void WriteJTAGModeword(CASIC::Name device, uint32_t data);

  // Status packet registers (0x84000000), reg is a byte offset 0x00-0x3C.
  uint32_t ReadRegister(unsigned reg);

  // IPCIDevice
  uint32_t ReadPCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const;
  void WritePCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, uint32_t data);

  // Tile generator register 0x0C was written: the board may start a frame.
  void TilegenDrawFrame(uint32_t flags);

  void Reset(void);
  void AttachRenderer(IRender3D *Render3DPtr);

  // 0x10, 0x15, 0x20 or 0x21 (anything else is treated as 0x10).
  void SetStepping(int stepping);

  // vromPtr: the video ROM as little endian 32-bit words. BusObjectPtr: the
  // bus the DMA engine copies on. dmaIRQBit: the IRQ raised when a DMA ends.
  Result Init(const uint8_t *vromPtr, IBus *BusObjectPtr, CIRQ *IRQObjectPtr, unsigned dmaIRQBit);

  CReal3D(const Util::Config::Node &config);
  ~CReal3D(void);

private:
  struct PendingUpload
  {
    unsigned level, x, y, width, height;
  };

  // Memory helpers
  uint32_t *Words(uint32_t byteOffset) const;
  void Touch(uint32_t byteOffset);
  uint32_t CopyChangedPages(bool everything);
  void BufferedWrite(uint32_t &blockAt, uint32_t regionStart, uint32_t wordAddr, uint32_t data);
  void ReplayBuffered(uint32_t lastBlock, uint32_t regionStart, uint32_t destOffset);

  // Textures
  void DecodeTextureUpload(uint32_t header, const uint16_t *src);
  unsigned WriteTexels(unsigned level, unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *src, bool wide, bool toLow, bool toHigh);
  void TellRenderer(unsigned level, unsigned x, unsigned y, unsigned w, unsigned h);
  void DrainTextureFIFO(void);

  // Frame control
  bool PingPongChanged(void) const;
  void StartFrame(void);
  void RunDMA(void);

  const Util::Config::Node &m_config;
  const bool m_threaded;

  IRender3D *m_render = nullptr;
  const uint32_t *m_vrom = nullptr;
  IBus *m_bus = nullptr;
  CIRQ *m_irq = nullptr;
  unsigned m_dmaIRQ = 0;
  int m_step = 0x10;
  uint32_t m_pciID = PCIID::Step1x;
  bool m_fifoErrorShown = false;

  // Board memory: low culling, high culling, polygon, texture RAM, texture
  // FIFO, in that order (the order the save state stores them in).
  std::unique_ptr<uint8_t[]> m_mem;
  std::unique_ptr<uint8_t[]> m_copy;      // render thread's copy (threaded only)
  std::vector<uint8_t> m_changed;         // one flag per 4 KB page of the copied part
  uint32_t m_fifoCount = 0;
  uint32_t m_vromPort[2] = { 0, 0 };
  uint32_t m_vromPortCount = 0;
  uint32_t m_cfg[4] = { 0, 0, 0, 0 };     // 0: ping-pong buffer size in words, 1: flags, 2: total buffer size

  std::vector<PendingUpload> m_uploads;
  std::vector<PendingUpload> m_uploadsForRender;

  // DMA engine
  uint32_t m_dmaSrc = 0, m_dmaDst = 0, m_dmaLen = 0, m_dmaResult = 0, m_dmaReserved = 0;
  uint8_t m_dmaStatus = 0, m_dmaCtrl = 0;

  // Frame state
  bool m_sceneDone = false;     // 0x88000000 written since the last ping-pong flip
  bool m_frameAsked = false;    // tile generator asked for a frame
  uint32_t m_pingPong = 0;
  uint32_t m_pingPongAtVBlank = 0;
  bool m_noRenderForFrame = false;
  uint64_t m_renderConfig[2] = { 0, 0 };  // kept for the save state layout
  uint32_t m_modeword[5] = { 0, 0, 0, 0, 0 };

  // Writes made after the ping-pong flip wait in update lists kept in low
  // culling RAM until the next vertical blank. These are word offsets of the
  // current list block from the start of memory, or kNoBlock.
  uint32_t m_polyBlock;
  uint32_t m_highBlock;
};

#endif  // INCLUDED_REAL3D_H
