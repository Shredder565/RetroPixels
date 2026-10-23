// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Model 3 main board: the PowerPC's address space (RAM, CROM, the
// Real3D, the tile generator, the I/O board, backup RAM, the security
// board, the PCI bridge, SCSI and the net board) and the frame loop that
// runs the PowerPC, the sound board, the drive board and the net board,
// optionally on threads of their own.

#ifndef INCLUDED_MODEL3_H
#define INCLUDED_MODEL3_H

#include "53C810.h"
#include "93C46.h"
#include "Crypto.h"
#include "IEmulator.h"
#include "JTAG.h"
#include "MPC10x.h"
#include "Real3D.h"
#include "RTC72421.h"
#include "SoundBoard.h"
#include "TileGen.h"
#include "DriveBoard/DriveBoard.h"
#include "CPU/PowerPC/ppc.h"
#include "Network/INetBoard.h"
#include "Util/NewConfig.h"
#include "Graphics/SuperAA.h"
#include "OSD/Thread.h"

// How long the parts of the last frame took, in milliseconds (for
// -dump-timings and the debugger).
struct FrameTimings
{
  UINT32 ppcTicks;      // PowerPC
  UINT32 syncSize;      // bytes copied for the render thread
  UINT32 syncTicks;
  UINT32 renderTicks;
  UINT32 sndTicks;      // sound board
  UINT32 drvTicks;      // drive board
  UINT32 netTicks;
  UINT32 frameTicks;    // the whole frame
  UINT64 frameId;
};

// One emulated Model 3. The PowerPC core is global, so only one can exist.
class CModel3: public IEmulator, public IBus, public IPCIDevice
{
public:
  explicit CModel3(Util::Config::Node &config);
  ~CModel3(void);

  // IEmulator
  Result Init(void);
  Result LoadGame(const Game &game, const ROMSet &rom_set);
  const Game &GetGame(void) const;
  void Reset(void);
  void RunFrame(void);
  void RenderFrame(void);
  bool PauseThreads(void);
  bool ResumeThreads(void);
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);
  void SaveNVRAM(CBlockFile *NVRAM);
  void LoadNVRAM(CBlockFile *NVRAM);
  void ClearNVRAM(void);
  void AttachRenderers(CRender2D *Render2DPtr, IRender3D *Render3DPtr, SuperAA *superAA);
  void AttachInputs(CInputs *InputsPtr);
  void AttachOutputs(COutputs *OutputsPtr);

  // IBus: the PowerPC's view of memory
  UINT8 Read8(UINT32 addr);
  UINT16 Read16(UINT32 addr);
  UINT32 Read32(UINT32 addr);
  UINT64 Read64(UINT32 addr);
  void Write8(UINT32 addr, UINT8 data);
  void Write16(UINT32 addr, UINT16 data);
  void Write32(UINT32 addr, UINT32 data);
  void Write64(UINT32 addr, UINT64 data);

  // IPCIDevice: PCI device 16 (seen by Daytona USA 2)
  UINT32 ReadPCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const;
  void WritePCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, UINT32 data);

  // The boards, for the debugger and the front end
  CSoundBoard *GetSoundBoard(void);
  CDriveBoard *GetDriveBoard(void);
  INetBoard *GetNetBoard(void);
  Util::Config::Node &GetConfig() { return m_config; }

  void DumpTimings(void);
  FrameTimings GetTimings(void);

private:
  // I/O board (F0040000): controls, EEPROM, lamps, drive board, ADC
  UINT8 ReadIO(unsigned reg);
  void WriteIO(unsigned reg, UINT8 data);
  UINT8 ReadButtons(void);         // reg 04
  UINT8 ReadGamePort1(void);       // reg 08
  UINT8 ReadGamePort2(void);       // reg 0C
  UINT8 ReadADC(void);             // reg 3C
  void GunCommand(UINT8 command);  // reg 24

  // Security board (F01A0000) and its RAM, read by the decryption device
  uint16_t ReadCryptRAM(uint32_t wordAddr);
  UINT32 ReadSecurity(unsigned reg);
  void WriteSecurity(unsigned reg, UINT32 data);

  // System registers (F0100000)
  void SelectCROMBank(unsigned value);
  UINT8 ReadSysReg(unsigned reg) const;
  void WriteSysReg(unsigned reg, UINT8 data);

  // Step 1.x address C0xxxxxx: the net board when it runs, else SCSI
  bool SCSIAtC0(void) const;

  // A frame's work for each board
  void RunMainBoardFrame(void);
  void SyncGPUs(void);
  bool RunSoundBoardFrame(void);
  void RunDriveBoardFrame(void);
  void RunNetBoardFrame(void);

  // Threads
  bool StartThreads(void);
  bool StopThreads(void);
  void DeleteThreadObjects(void);
  bool WaitForThreads(void);       // with m_notifyLock held
  bool ThreadError(const char *where);
  int WaitForWork(CSemaphore *go, bool *running);
  bool FinishWork(bool *running, bool *done);
  int MainBoardThread(void);
  int SoundBoardThread(void);      // paced by the audio callback
  int DriveBoardThread(void);
  bool WakeSoundBoardThread(void);
  static int MainBoardThreadEntry(void *self);
  static int SoundBoardThreadEntry(void *self);
  static int DriveBoardThreadEntry(void *self);
  static void AudioCallback(void *self);

  Util::Config::Node &m_config;
  bool m_multiThreaded;
  bool m_gpuMultiThreaded;

  Game m_game;
  int m_stepping = 0;             // 0x10, 0x15, 0x20 or 0x21

  CInputs *m_inputs = nullptr;
  COutputs *m_outputs = nullptr;

  // I/O board state
  UINT8 m_ioBank = 0;             // reg 00: bank select and EEPROM lines
  UINT8 m_serial1 = 0;            // serial FIFO 1 and 2 (light guns)
  UINT8 m_serial2 = 0;
  UINT8 m_gunReg = 0;
  int m_adcChannel = 0;
  UINT8 m_ioOut[2] = { 0, 0 };    // regs 10 (drive board) and 14 (lamps)
  UINT8 m_midiControl = 0;        // sound board MIDI control port

  // Memory, one allocation (see the layout in Model3.cpp)
  UINT8 *m_pool = nullptr;
  UINT8 *m_ram = nullptr;         // 8 MB, stored as little-endian words
  UINT8 *m_crom = nullptr;        // 8 MB fixed CROM, then 128 MB banked
  UINT8 *m_vrom = nullptr;        // 64 MB, the Real3D's
  UINT8 *m_soundROM = nullptr;    // 512 KB, 68K program
  UINT8 *m_sampleROM = nullptr;   // 16 MB
  UINT8 *m_dsbROM = nullptr;      // 128 KB, DSB program
  UINT8 *m_mpegROM = nullptr;     // 16 MB, DSB music
  UINT8 *m_backupRAM = nullptr;   // 128 KB, battery backed
  UINT8 *m_securityRAM = nullptr; // 128 KB
  UINT8 *m_driveROM = nullptr;    // 64 KB, drive board program
  UINT8 *m_netRAM = nullptr;      // 64 KB
  UINT8 *m_netBuffer = nullptr;   // 128 KB

  UINT8 *m_cromBank = nullptr;    // the banked CROM mapped at FF000000
  unsigned m_cromBankReg = 0;

  bool m_securityFirstRead = true;
  unsigned m_securityPtr = 0;     // kept for the save state layout

  PPC_FETCH_REGION m_fetch[3];

  // Threads
  bool m_gpusReady = false;
  bool m_threadsStarted = false;
  bool m_pause = false;
  bool m_stop = false;
  CThread *m_mainThread = nullptr;
  CThread *m_soundThread = nullptr;
  CThread *m_driveThread = nullptr;
  bool m_mainRunning = false, m_mainDone = false;
  bool m_soundRunning = false, m_soundDone = false;
  bool m_driveRunning = false, m_driveDone = false;
  bool m_soundWake = false;
  CSemaphore *m_mainGo = nullptr;
  CSemaphore *m_driveGo = nullptr;
  CMutex *m_soundWakeLock = nullptr;
  CCondVar *m_soundWakeCond = nullptr;
  CMutex *m_notifyLock = nullptr;
  CCondVar *m_notifyCond = nullptr;

  FrameTimings m_timings;

  // Devices
  CIRQ m_irq;
  CMPC10x m_pciBridge;
  CPCIBus m_pciBus;
  C53C810 m_scsi;
  CRTC72421 m_rtc;
  C93C46 m_eeprom;
  CTileGen m_tileGen;
  CReal3D m_gpu;
  CSoundBoard m_soundBoard;
  CDSB *m_dsb = nullptr;
  CDriveBoard *m_driveBoard = nullptr;
  CCrypto m_crypto;
  CJTAG m_jtag;
  SuperAA *m_superAA = nullptr;
  INetBoard *m_netBoard = nullptr;
  bool m_runNetBoard = false;
};

#endif  // INCLUDED_MODEL3_H
