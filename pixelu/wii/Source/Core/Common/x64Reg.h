// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

namespace Gen
{
// Register numbers as the x86-64 encodings use them (ModRM/SIB fields plus the REX extension
// bit). Several names share a number: the width of an access is given by the instruction, not by
// the register name.
//
// Two flag bits sit above the 4-bit hardware number:
//   0x100 on the legacy high-byte registers AH/CH/DH/BH (hardware numbers 4-7 without a REX
//         prefix), and on the 256-bit YMM registers.
enum X64Reg
{
  // 64-bit general purpose registers
  RAX = 0,
  RCX = 1,
  RDX = 2,
  RBX = 3,
  RSP = 4,
  RBP = 5,
  RSI = 6,
  RDI = 7,
  R8 = 8,
  R9 = 9,
  R10 = 10,
  R11 = 11,
  R12 = 12,
  R13 = 13,
  R14 = 14,
  R15 = 15,

  // 32-bit names
  EAX = RAX,
  ECX = RCX,
  EDX = RDX,
  EBX = RBX,
  ESP = RSP,
  EBP = RBP,
  ESI = RSI,
  EDI = RDI,

  // 16-bit names
  AX = RAX,
  CX = RCX,
  DX = RDX,
  BX = RBX,
  SP = RSP,
  BP = RBP,
  SI = RSI,
  DI = RDI,

  // Low bytes. SPL/BPL/SIL/DIL need a REX prefix; the emitter adds one when it sees them.
  AL = RAX,
  CL = RCX,
  DL = RDX,
  BL = RBX,
  SPL = RSP,
  BPL = RBP,
  SIL = RSI,
  DIL = RDI,

  // Legacy high bytes (only encodable without a REX prefix)
  AH = 0x100 | 4,
  CH = 0x100 | 5,
  DH = 0x100 | 6,
  BH = 0x100 | 7,

  // 128-bit vector registers
  XMM0 = 0,
  XMM1 = 1,
  XMM2 = 2,
  XMM3 = 3,
  XMM4 = 4,
  XMM5 = 5,
  XMM6 = 6,
  XMM7 = 7,
  XMM8 = 8,
  XMM9 = 9,
  XMM10 = 10,
  XMM11 = 11,
  XMM12 = 12,
  XMM13 = 13,
  XMM14 = 14,
  XMM15 = 15,

  // 256-bit vector registers (VEX.L = 1)
  YMM0 = 0x100 | 0,
  YMM1 = 0x100 | 1,
  YMM2 = 0x100 | 2,
  YMM3 = 0x100 | 3,
  YMM4 = 0x100 | 4,
  YMM5 = 0x100 | 5,
  YMM6 = 0x100 | 6,
  YMM7 = 0x100 | 7,
  YMM8 = 0x100 | 8,
  YMM9 = 0x100 | 9,
  YMM10 = 0x100 | 10,
  YMM11 = 0x100 | 11,
  YMM12 = 0x100 | 12,
  YMM13 = 0x100 | 13,
  YMM14 = 0x100 | 14,
  YMM15 = 0x100 | 15,

  INVALID_REG = 0xFFFFFFFF
};

}  // namespace Gen
