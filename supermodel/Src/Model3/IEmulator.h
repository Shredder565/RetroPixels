// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// IEmulator: what the front end (OSD/SDL/Main.cpp) drives. CModel3 is the
// real machine; CModel3GraphicsState only replays the video state of a
// save state.

#ifndef INCLUDED_IEMULATOR_H
#define INCLUDED_IEMULATOR_H

class CBlockFile;
struct Game;
struct ROMSet;
class CRender2D;
class IRender3D;
class CInputs;
class COutputs;
class SuperAA;

class IEmulator
{
public:
  // Save states. Never called from inside RunFrame(); the front end pauses
  // the emulator's threads (PauseThreads()) around them.
  virtual void SaveState(CBlockFile *SaveState) = 0;
  virtual void LoadState(CBlockFile *SaveState) = 0;

  // Battery-backed memory (NVRAM files).
  virtual void SaveNVRAM(CBlockFile *NVRAM) = 0;
  virtual void LoadNVRAM(CBlockFile *NVRAM) = 0;
  virtual void ClearNVRAM(void) = 0;

  // Emulates one video frame (RunFrame() also renders it).
  virtual void RunFrame(void) = 0;
  virtual void RenderFrame(void) = 0;

  // Resets the machine; NVRAM is kept.
  virtual void Reset(void) = 0;

  // The loaded game (an empty Game before LoadGame()).
  virtual const Game &GetGame(void) const = 0;

  // Takes the game's ROMs and sets the machine up for its stepping.
  // Prints its own errors and returns FAIL on any.
  virtual Result LoadGame(const Game &game, const ROMSet &rom_set) = 0;

  // Hooks the emulator up to the front end's renderers, inputs and outputs.
  virtual void AttachRenderers(CRender2D *Render2DPtr, IRender3D *Render3DPtr, SuperAA *superAA) = 0;
  virtual void AttachInputs(CInputs *InputsPtr) = 0;
  virtual void AttachOutputs(COutputs *OutputsPtr) = 0;

  // One-time setup, before anything else (the command line has not been
  // applied yet). Prints its own errors and returns FAIL on any.
  virtual Result Init(void) = 0;

  // Stops the emulator's worker threads at a safe point and lets them go
  // again. Both return false on a threading error.
  virtual bool PauseThreads(void) = 0;
  virtual bool ResumeThreads(void) = 0;

  virtual ~IEmulator(void)
  {
  }
};

#endif // INCLUDED_IEMULATOR_H
