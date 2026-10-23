// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Z80.h: Zilog Z80 interpreter, used by the Model 3's Digital Sound Board
// and drive boards.
//
// Timing is counted per instruction. Memory and I/O go through an IBus
// (Read8/Write8 for memory, IORead8/IOWrite8 with an 8-bit port number).

#ifndef INCLUDED_Z80_H
#define INCLUDED_Z80_H

#include "Types.h"
#include "CPU/Bus.h"
#include "BlockFile.h"

// Values an interrupt acknowledge callback returns in interrupt mode 0: the
// RST instruction the device puts on the bus.
#define Z80_INT_RST_00  0xC7
#define Z80_INT_RST_08  0xCF
#define Z80_INT_RST_10  0xD7
#define Z80_INT_RST_18  0xDF
#define Z80_INT_RST_20  0xE7
#define Z80_INT_RST_28  0xEF
#define Z80_INT_RST_30  0xF7
#define Z80_INT_RST_38  0xFF

class CZ80
{
public:
  // Runs for at least numCycles cycles (or until HALT) and returns the
  // number of cycles used. Interrupts are taken between instructions.
  int Run(int numCycles);

  // Requests a non-maskable interrupt (taken after the next instruction).
  void TriggerNMI(void);

  // Drives the maskable interrupt line.
  void SetINT(bool state);

  UINT16 GetPC(void) const;

  void Reset(void);
  void SaveState(CBlockFile *StateFile, const char *name);
  void LoadState(CBlockFile *StateFile, const char *name);

  // Attaches the bus and the interrupt acknowledge callback. In mode 0 the
  // callback returns an RST opcode (Z80_INT_RST_xx), in mode 2 the low byte
  // of the vector table address; in mode 1 it is only told about the
  // interrupt. Without a callback, mode 0 and 2 interrupts are dropped and
  // the line is released.
  void Init(IBus *BusPtr, int (*INTF)(CZ80 *Z80));

  CZ80(void);
  ~CZ80(void);

private:
  enum class Index : UINT8 { HL, IX, IY };

  struct RegisterBank
  {
    UINT16 bc;
    UINT16 de;
    UINT16 hl;
  };

  // Main/alternate register banks and which of each is in use. The
  // order of these members is also the save state order.
  RegisterBank m_bank[2];
  UINT16  m_afPair[2];
  UINT16  m_iAndR;      // I in the high byte, R in the low byte
  UINT16  m_ixReg;
  UINT16  m_iyReg;
  UINT16  m_spReg;
  UINT16  m_pcReg;
  UINT8   m_iffBits;    // bit 0: IFF1, bit 1: IFF2
  UINT8   m_intMode;    // 0, 1 or 2
  int     m_bankInUse;
  int     m_afInUse;

  bool    m_nmiPending;
  bool    m_intAsserted;

  IBus    *m_bus;
  int     (*m_intAck)(CZ80 *Z80);

  int     m_budget;     // cycles left in the current Run()
  bool    m_halted;     // HALT executed: leave Run()

  // Register access
  UINT16 &BC() { return m_bank[m_bankInUse].bc; }
  UINT16 &DE() { return m_bank[m_bankInUse].de; }
  UINT16 &HL() { return m_bank[m_bankInUse].hl; }
  UINT16 &AF() { return m_afPair[m_afInUse]; }
  UINT8 A() { return (UINT8) (AF() >> 8); }
  UINT8 F() { return (UINT8) AF(); }
  void SetA(unsigned v) { AF() = (UINT16) ((AF() & 0x00FF) | ((v & 0xFF) << 8)); }
  void SetF(unsigned v) { AF() = (UINT16) ((AF() & 0xFF00) | (v & 0xFF)); }
  UINT16 &IndexReg(Index idx);
  UINT8 GetReg8(unsigned r, Index idx);
  void SetReg8(unsigned r, Index idx, unsigned v);
  UINT16 GetPair(unsigned p, Index idx);
  void SetPair(unsigned p, Index idx, unsigned v);

  // Bus helpers
  UINT8 Fetch8();
  UINT16 Fetch16();
  UINT16 Read16(unsigned addr);
  void Write16(unsigned addr, unsigned v);
  void Push(unsigned v);
  UINT16 Pop();
  bool Condition(unsigned cc);

  // Flag helpers
  void Alu(unsigned op, unsigned operand);
  unsigned Increment(unsigned v);
  unsigned Decrement(unsigned v);
  void AddToPair(UINT16 &dst, unsigned src);
  void AddWithCarryHL(unsigned src);
  void SubWithCarryHL(unsigned src);
  unsigned ShiftOrRotate(unsigned kind, unsigned v);
  void BitTest(unsigned bit, unsigned v, bool fromRegister);
  void Daa();
  void BlockCompareFlags(unsigned a, unsigned value, bool more);

  // Execution
  void Step();
  void ExecMain(unsigned op, Index idx);
  void ExecBitOps(Index idx);
  void ExecExtended();
  void ExecBlock(unsigned op);
  void ServiceInterrupts();
};

#endif // INCLUDED_Z80_H
