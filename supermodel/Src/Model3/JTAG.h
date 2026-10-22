// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The video board's IEEE 1149.1 (JTAG) scan chain, driven by the game
// through system register F010000C (TCK bit 6, TDI bit 5, TMS bit 2, TRST
// bit 7) and read back through F0100010 (TDO bit 5).
//
// The chain holds the Real3D ASICs (Mercury, Venus, Earth, Mars, Jupiter)
// and, on Step 2.x boards, the 3D-RAM chips. Games read the ASICs' ID codes
// to identify the board and write each ASIC's 32-bit "modeword" register,
// which configures the renderer (CReal3D::WriteJTAGModeword()). The 3D-RAMs
// only ever get BYPASS.

#ifndef INCLUDED_JTAG_H
#define INCLUDED_JTAG_H

#include "BlockFile.h"
#include <bitset>
#include <cstdint>

class CReal3D;

class CASIC
{
public:
  // Which Real3D ASIC (indexes CReal3D's modeword table). Dummy marks the
  // second and later ASICs of a type, whose modewords are not forwarded.
  enum class Name
  {
    Mercury = 0,
    Venus   = 1,
    Earth   = 2,
    Mars    = 3,
    Jupiter = 4,
    Dummy   = -1
  };
};

class CJTAG
{
public:
  // Save state block "JTAG2".
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *LoadState);

  // TDO.
  bool Read() const;

  // New pin levels. TRST is active low.
  void Write(bool tck, bool tms, bool tdi, bool trst);

  void Reset();

  // Selects the chain (10 devices on Step 1.x, 17 on 2.x) and the ID codes
  // of the board revision (0x10, 0x15, 0x20, 0x21).
  void SetStepping(int stepping);

  CJTAG(CReal3D &real3D);

private:
  // TAP controller states, numbered as stored in save states.
  enum class State
  {
    TestLogicReset, RunTestIdle,
    SelectDRScan, CaptureDR, ShiftDR, Exit1DR, PauseDR, Exit2DR, UpdateDR,
    SelectIRScan, CaptureIR, ShiftIR, Exit1IR, PauseIR, Exit2IR, UpdateIR
  };

  static constexpr unsigned kMaxBits = 262;
  static constexpr unsigned kChainLength = 17;

  struct Device
  {
    bool isASIC = false;
    CASIC::Name name = CASIC::Name::Dummy;
    std::bitset<kMaxBits> shift;
    uint32_t length = 1;        // bits in the selected register
    uint8_t ir = 0;             // instruction register
    uint32_t idCode = 0;        // ASICs only
    uint32_t modeword = 0;      // ASICs only
  };

  static State Next(State s, bool tms);
  void DeviceReset(Device &d);
  void DeviceCaptureIR(Device &d);
  void DeviceCaptureDR(Device &d);
  void DeviceUpdateIR(Device &d);
  void DeviceUpdateDR(Device &d);
  static bool DeviceShift(Device &d, bool tdi);
  void SaveDevice(CBlockFile *f, const Device &d) const;
  void LoadDevice(CBlockFile *f, Device &d);

  CReal3D &m_real3D;
  State m_state = State::TestLogicReset;
  bool m_lastTck = false;
  bool m_tdo = false;
  Device m_chain[kChainLength];
  uint32_t m_numDevices = 0;    // devices present on this board
};

#endif // INCLUDED_JTAG_H
