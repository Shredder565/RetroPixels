// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The part of a Z80 CTC (counter/timer circuit) the drive boards use: four
// channels that take control words and time constants, and the rate a
// channel's timer runs at. Counting and interrupts are left to the board.

#pragma once

#include "Types.h"
#include "BlockFile.h"

class Z80CTC
{
public:
  // A byte written to a channel's port: a control word (bit 0 set) or the
  // time constant a control word asked for.
  void Write(UINT32 channel, UINT8 value);
  // How often the channel's timer fires, given the clock (0 if the mode is
  // not one this models).
  UINT32 CalcFrequency(UINT32 channel, UINT32 inputFrequency);
  bool TimerRunning(UINT32 channel) const;
  void Reset();
  void SaveState(CBlockFile *saveState) const;
  void LoadState(CBlockFile *saveState);

private:
  // Saved as raw bytes, 12 per channel: keep the members, their order and
  // their types as they are.
  struct Channel
  {
    UINT8 control = 0;           // last control word
    UINT8 timeConstant = 0;      // (a constant of 0 means 256, which this stores as 0)
    UINT16 counter = 0;
    bool interrupt = false;      // control bit 7
    bool counterMode = false;    // bit 6 (else timer mode)
    bool prescale256 = false;    // bit 5 (else 16)
    bool risingEdge = false;     // bit 4
    bool waitForTrigger = false; // bit 3 (else starts when the constant is loaded)
    bool expectConstant = false; // bit 2: the next byte is a time constant
    bool running = false;
    UINT8 reserved = 0;          // the padding byte, kept zero
  };
  static_assert(sizeof(Channel) == 12, "the save state stores 12 bytes per channel");

  Channel m_channel[4];
};
