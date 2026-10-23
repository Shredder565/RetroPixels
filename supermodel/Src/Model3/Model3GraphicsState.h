// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// A stand-in for CModel3 that only shows a saved video state (-gfx-state):
// it loads the tile generator's and the Real3D's state from a save state
// file on every reset and renders it each frame. Nothing else runs.

#ifndef INCLUDED_MODEL3GRAPHICSSTATE_H
#define INCLUDED_MODEL3GRAPHICSSTATE_H

#include "BlockFile.h"
#include "Game.h"
#include "ROMSet.h"
#include "CPU/Bus.h"
#include "Model3/IEmulator.h"
#include "Model3/IRQ.h"
#include "Model3/Real3D.h"
#include "Model3/TileGen.h"
#include "OSD/Logger.h"
#include "OSD/Video.h"
#include "Util/NewConfig.h"

class CModel3GraphicsState: public IEmulator, public IBus
{
public:
  CModel3GraphicsState(const Util::Config::Node &config, const std::string &filePath)
    : m_stateFile(filePath),
      m_tileGen(config),
      m_real3D(config)
  {
  }

  virtual ~CModel3GraphicsState(void)
  {
  }

  Result Init(void) override
  {
    m_vrom.reset(new uint8_t[64 * 0x100000], std::default_delete<uint8_t[]>());
    m_irq.Init();
    if (m_tileGen.Init(&m_irq) != Result::OKAY)
      return Result::FAIL;
    return m_real3D.Init(m_vrom.get(), this, &m_irq, 0x100);
  }

  Result LoadGame(const Game &game, const ROMSet &rom_set) override
  {
    m_game = game;
    // VROM as CModel3 maps it
    ROM vrom = rom_set.get_rom("vrom");
    if (vrom.size <= 32 * 0x100000)
    {
      vrom.CopyTo(m_vrom.get(), 32 * 100000);
      vrom.CopyTo(m_vrom.get() + 32 * 0x100000, 32 * 0x100000);
    }
    else
      vrom.CopyTo(m_vrom.get(), 64 * 0x100000);
    m_real3D.SetStepping(((m_game.stepping[0] - '0') << 4) | (m_game.stepping[2] - '0'));
    return Result::OKAY;
  }

  const Game &GetGame(void) const override
  {
    return m_game;
  }

  void Reset(void) override
  {
    CBlockFile file;
    if (file.Load(m_stateFile.c_str()) != Result::OKAY)
    {
      ErrorLog("Unable to load state from '%s'.", m_stateFile.c_str());
      return;
    }
    LoadState(&file);
    file.Close();
  }

  void LoadState(CBlockFile *SaveState) override
  {
    m_real3D.LoadState(SaveState);
    m_tileGen.LoadState(SaveState);
  }

  void RunFrame(void) override
  {
    RenderFrame();
  }

  void RenderFrame(void) override
  {
    BeginFrameVideo();
    m_tileGen.BeginFrame();
    m_real3D.BeginFrame();
    m_real3D.RenderFrame();
    m_real3D.EndFrame();
    m_tileGen.EndFrame();
    EndFrameVideo();
  }

  void AttachRenderers(CRender2D *render2D, IRender3D *render3D, SuperAA *) override
  {
    m_tileGen.AttachRenderer(render2D);
    m_real3D.AttachRenderer(render3D);
  }

  // Nothing else to do.
  void SaveState(CBlockFile *) override {}
  void SaveNVRAM(CBlockFile *) override {}
  void LoadNVRAM(CBlockFile *) override {}
  void ClearNVRAM(void) override {}
  void AttachInputs(CInputs *) override {}
  void AttachOutputs(COutputs *) override {}
  bool PauseThreads(void) override { return true; }
  bool ResumeThreads(void) override { return true; }

private:
  const std::string m_stateFile;
  std::shared_ptr<uint8_t> m_vrom;
  Game m_game;
  CIRQ m_irq;
  CTileGen m_tileGen;
  CReal3D m_real3D;
};

#endif  // INCLUDED_MODEL3GRAPHICSSTATE_H
