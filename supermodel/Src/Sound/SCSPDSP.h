// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The effect DSP inside each SCSP (used only by SCSP.cpp).
//
// Every 44.1 kHz sample the DSP runs its microprogram (up to 128 steps of
// four 16-bit words) once. Inputs are the 16 mixer sends (MIXS, fed by the
// voices), memory/temporary registers and the two external inputs; outputs
// are the 16 effect registers (EFREG), which the chip mixes back in with
// each slot's EFSDL/EFPAN. Sound RAM serves as a ring buffer (delay line),
// stored as 24-bit values packed to 16 bits (4-bit exponent, 11-bit
// mantissa) unless a step says otherwise.

#ifndef INCLUDED_SCSPDSP_H
#define INCLUDED_SCSPDSP_H

#include "Types.h"

namespace ScspDsp
{
  // Words of the program area, laid out as on the chip's register map
  // (0x700-0xBFF): 64 coefficients, 32 memory addresses, 128x4 microcode.
  constexpr unsigned kCoefWords = 64;
  constexpr unsigned kAddrWords = 32;
  constexpr unsigned kCodeWords = 128 * 4;
  constexpr unsigned kCoefBase = 0;
  constexpr unsigned kAddrBase = kCoefBase + kCoefWords;
  constexpr unsigned kCodeBase = kAddrBase + kAddrWords;
  constexpr unsigned kProgramWords = kCodeBase + kCodeWords;

  struct State
  {
    UINT16 *ram;             // the chip's sound RAM, as 16-bit words
    unsigned ringBase;       // RBP: ring buffer start in 4 K-word units
    unsigned ringWords;      // RBL decoded: ring buffer length in words
    UINT16 program[kProgramWords];
    INT32 temp[128];         // TEMP: 24-bit work registers (a ring of 128)
    INT32 mems[32];          // MEMS: values read from memory
    unsigned ringOffset;     // DEC: counts down once per sample
    INT32 mixs[16];          // MIXS: voice sends, summed over a sample
    INT16 exts[2];           // EXTS: external inputs (not connected on Model 3)
    INT16 efreg[16];         // EFREG: effect outputs
    bool idle;               // no program loaded yet
    int steps;               // length of the loaded program

    INT16 *Coefficients() { return reinterpret_cast<INT16 *>(&program[kCoefBase]); }
    UINT16 *Addresses() { return &program[kAddrBase]; }
    UINT16 *Code() { return &program[kCodeBase]; }
  };

  void Reset(State &dsp);
  // Adds a voice's send to mixer input 'sel'.
  void Send(State &dsp, INT32 value, int sel);
  // Runs the program for one sample.
  void RunSample(State &dsp);
  // Called when the last microcode words are written: (re)starts the DSP.
  void Start(State &dsp);
}

#endif // INCLUDED_SCSPDSP_H
