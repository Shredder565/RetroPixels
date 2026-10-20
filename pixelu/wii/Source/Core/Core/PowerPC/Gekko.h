// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Register and instruction-word formats of the Gekko/Broadway CPU (a
// PowerPC 750CL derivative), as described in IBM's 750CL user manual and the
// PowerPC Operating Environment Architecture. Bit positions below count from
// the least significant bit (bit 0 = 2^0), which is the reverse of the IBM
// manuals' big-endian numbering.

#pragma once

#include "Common/BitField.h"
#include "Common/CommonTypes.h"
#include "Common/FPURoundMode.h"

// ---------------------------------------------------------------------------
// Instruction word
// ---------------------------------------------------------------------------

// Every view of a 32-bit instruction word the decoders need. The primary
// opcode is always the top six bits; the views below differ in how they cut
// up the remaining 26 bits. Several fields exist under more than one name
// (OPCD, OPCD_2, ... or RA, RA_2, ...) because each anonymous struct must
// spell out its own copy.
union UGeckoInstruction
{
  u32 hex = 0;

  UGeckoInstruction() = default;
  UGeckoInstruction(u32 hex_) : hex(hex_) {}

  // X/XO-form: rD (or rS/frD), rA, rB, extended opcode, record bit.
  struct
  {
    u32 Rc : 1;
    u32 SUBOP10 : 10;
    u32 RB : 5;
    u32 RA : 5;
    u32 RD : 5;
    u32 OPCD : 6;
  };
  // X-form with rS as the source in bits 21-25 (stores, logical ops).
  struct
  {
    u32 Rc_2 : 1;
    u32 : 20;
    u32 RS : 5;
    u32 OPCD_3 : 6;
  };
  // XO-form overflow-enable bit.
  struct
  {
    u32 : 10;
    u32 OE : 1;
    u32 : 21;
  };
  // XO-form overflow-enable bit together with the split SPR number used by
  // mfspr/mtspr (the halves are swapped in the encoding).
  struct
  {
    u32 : 10;
    u32 OE_3 : 1;
    u32 SPRU : 5;
    u32 SPRL : 5;
    u32 : 11;
  };
  // The raw 10-bit SPR field (halves still swapped).
  struct
  {
    u32 : 11;
    u32 SPR : 10;
    u32 : 11;
  };
  // mftb: time base register number, raw and split.
  struct
  {
    u32 : 11;
    u32 TBR : 10;
    u32 : 11;
  };
  struct
  {
    u32 : 11;
    u32 TBRU : 5;
    u32 TBRL : 5;
    u32 : 11;
  };

  // D-form with a signed 16-bit immediate; tw/twi trap condition.
  struct
  {
    s32 SIMM_16 : 16;
    u32 : 5;
    u32 TO : 5;
    u32 OPCD_2 : 6;
  };
  // D-form with an unsigned 16-bit immediate.
  struct
  {
    u32 UIMM : 16;
    u32 : 10;
    u32 OPCD_4 : 6;
  };

  // I-form (b): 24-bit word displacement, absolute and link bits.
  struct
  {
    u32 LK : 1;
    u32 AA : 1;
    u32 LI : 24;
    u32 OPCD_5 : 6;
  };
  // B-form (bc): 14-bit word displacement, condition bit, branch options.
  struct
  {
    u32 LK_2 : 1;
    u32 AA_2 : 1;
    u32 BD : 14;
    u32 BI : 5;
    u32 BO : 5;
    u32 OPCD_6 : 6;
  };
  // XL-form (bclr, bcctr and the CR logic ops).
  struct
  {
    u32 LK_3 : 1;
    u32 XO : 10;
    u32 : 5;
    u32 BI_2 : 5;
    u32 BO_2 : 5;
    u32 OPCD_7 : 6;
  };

  // Compare forms: crfD in bits 23-25, the L bit in bit 21.
  struct
  {
    u32 : 11;
    u32 RB_2 : 5;
    u32 RA_2 : 5;
    u32 L : 1;
    u32 : 1;
    u32 CRFD : 3;
    u32 OPCD_8 : 6;
  };
  struct
  {
    s32 SIMM_16_2 : 16;
    u32 RA_3 : 5;
    u32 L_2 : 1;
    u32 : 1;
    u32 CRFD_2 : 3;
    u32 OPCD_9 : 6;
  };
  struct
  {
    u32 UIMM_2 : 16;
    u32 RA_4 : 5;
    u32 L_3 : 1;
    u32 : 1;
    u32 CRFD_3 : 3;
    u32 OPCD_A : 6;
  };
  struct
  {
    u32 : 1;
    u32 SUBOP10_2 : 10;
    u32 RB_5 : 5;
    u32 RA_5 : 5;
    u32 L_4 : 1;
    u32 : 1;
    u32 CRFD_4 : 3;
    u32 OPCD_B : 6;
  };
  // mcrf/mcrfs/mcrxr: source field in bits 18-20, destination in 23-25.
  struct
  {
    u32 : 18;
    u32 CRFS : 3;
    u32 : 2;
    u32 CRFD_5 : 3;
    u32 : 6;
  };

  // mtsr/mfsr: segment register number.
  struct
  {
    u32 : 16;
    u32 SR : 4;
    u32 : 1;
    u32 RS_2 : 5;
    u32 OPCD_C : 6;
  };

  // A-form (opcode 4/59/63 arithmetic): five-bit extended opcode and the
  // third source register frC.
  struct
  {
    u32 Rc_4 : 1;
    u32 SUBOP5 : 5;
    u32 RC : 5;
    u32 : 5;
    u32 RA_6 : 5;
    u32 RD_2 : 5;
    u32 OPCD_D : 6;
  };
  // Floating-point register names for the same positions.
  struct
  {
    u32 : 6;
    u32 FC : 5;
    u32 FB : 5;
    u32 FA : 5;
    u32 FS : 5;
    u32 : 6;
  };

  // M-form (rlwinm, rlwimi, rlwnm): shift amount and mask bounds.
  struct
  {
    u32 Rc_3 : 1;
    u32 ME : 5;
    u32 MB : 5;
    u32 SH : 5;
    u32 : 16;
  };

  // CR logic: destination and source CR bit numbers.
  struct
  {
    u32 : 11;
    u32 CRBB : 5;
    u32 CRBA : 5;
    u32 CRBD : 5;
    u32 : 6;
  };

  // mtcrf field mask, and frD for the floating-point forms.
  struct
  {
    u32 : 12;
    u32 CRM : 8;
    u32 : 1;
    u32 FD : 5;
    u32 : 6;
  };
  // mtfsf field mask.
  struct
  {
    u32 : 17;
    u32 FM : 8;
    u32 : 7;
  };

  // Paired-single quantized loads and stores: the D-form variants (psq_l,
  // psq_st) keep W and I at bits 15 and 12-14 with a 12-bit offset; the
  // indexed variants keep Wx and Ix at bits 10 and 7-9 next to a six-bit
  // extended opcode.
  struct
  {
    u32 : 1;
    u32 SUBOP6 : 6;
    u32 Ix : 3;
    u32 Wx : 1;
    u32 : 1;
    u32 I : 3;
    u32 W : 1;
    u32 : 16;
  };
  struct
  {
    s32 SIMM_12 : 12;
    u32 : 20;
  };

  // lswi/stswi byte count (0 means 32).
  struct
  {
    u32 : 11;
    u32 NB : 5;
  };
};

// Only meaningful for b, bc, bclr and bcctr. A BO of the form 1z1zz ignores
// both the CTR and the condition, so the branch is always taken.
constexpr bool BranchIsConditional(UGeckoInstruction inst)
{
  if (inst.OPCD == 18)
    return false;
  constexpr u32 always = 0b10100;
  return (inst.BO & always) != always;
}

// The rotate-and-mask instructions' mask: ones from big-endian bit mb to bit
// me inclusive, wrapping around when me < mb.
inline u32 MakeRotationMask(u32 mb, u32 me)
{
  const u32 from_mb = 0xFFFFFFFFu >> mb;        // ones at bits mb..31
  const u32 after_me = 0xFFFFFFFFu >> me >> 1;  // ones at bits me+1..31
  const u32 run = from_mb ^ after_me;
  return me < mb ? ~run : run;
}

// ---------------------------------------------------------------------------
// Special registers
// ---------------------------------------------------------------------------

// Data types a graphics quantization register can select.
enum EQuantizeType : u32
{
  QUANTIZE_FLOAT = 0,
  QUANTIZE_INVALID1 = 1,
  QUANTIZE_INVALID2 = 2,
  QUANTIZE_INVALID3 = 3,
  QUANTIZE_U8 = 4,
  QUANTIZE_U16 = 5,
  QUANTIZE_S8 = 6,
  QUANTIZE_S16 = 7,
};

// GQR0-7: store type/scale in the low half, load type/scale in the high
// half.
union UGQR
{
  BitField<0, 3, EQuantizeType> st_type;
  BitField<8, 6, u32> st_scale;
  BitField<16, 3, EQuantizeType> ld_type;
  BitField<24, 6, u32> ld_scale;

  u32 Hex = 0;

  UGQR() = default;
  explicit UGQR(u32 hex_) : Hex{hex_} {}
};

// XER flag positions. The emulator keeps CA, OV and SO in separate bytes of
// the CPU state (see PowerPC.h); these describe where they go in the real
// register and how the packed SO/OV byte is laid out.
#define XER_CA_SHIFT 29
#define XER_OV_SHIFT 30
#define XER_SO_SHIFT 31
#define XER_OV_MASK 1
#define XER_SO_MASK 2

union UReg_XER
{
  BitField<0, 7, u32> BYTE_COUNT;  // lswx/stswx byte count
  BitField<7, 1, u32> reserved_1;
  BitField<8, 8, u32> BYTE_CMP;
  BitField<16, 13, u32> reserved_2;
  BitField<29, 1, u32> CA;  // carry
  BitField<30, 1, u32> OV;  // overflow
  BitField<31, 1, u32> SO;  // summary overflow

  u32 Hex = 0;

  UReg_XER() = default;
  explicit UReg_XER(u32 hex_) : Hex{hex_} {}
};

// Machine state register.
union UReg_MSR
{
  BitField<0, 1, u32> LE;  // little-endian mode
  BitField<1, 1, u32> RI;  // recoverable interrupt
  BitField<2, 1, u32> PM;  // performance monitor marked mode
  BitField<3, 1, u32> reserved_1;
  BitField<4, 1, u32> DR;  // data address translation
  BitField<5, 1, u32> IR;  // instruction address translation
  BitField<6, 1, u32> IP;  // exception vectors at 0xFFFn_nnnn
  BitField<7, 1, u32> reserved_2;
  BitField<8, 1, u32> FE1;  // FP exception mode, low bit
  BitField<9, 1, u32> BE;   // branch trace
  BitField<10, 1, u32> SE;  // single-step trace
  BitField<11, 1, u32> FE0;  // FP exception mode, high bit
  BitField<12, 1, u32> MCHECK;  // machine check enable
  BitField<13, 1, u32> FP;      // floating point available
  BitField<14, 1, u32> PR;      // problem (user) state
  BitField<15, 1, u32> EE;      // external interrupts enabled
  BitField<16, 1, u32> ILE;     // little-endian mode in exception handlers
  BitField<17, 1, u32> reserved_3;
  BitField<18, 1, u32> POW;  // power management
  BitField<19, 13, u32> reserved_4;

  u32 Hex = 0;

  UReg_MSR() = default;
  explicit UReg_MSR(u32 hex_) : Hex{hex_} {}
};

// FPRF (result class and condition code) inside FPSCR.
#define FPRF_SHIFT 12
#define FPRF_WIDTH 5
#define FPRF_MASK (0x1F << FPRF_SHIFT)
#define FPCC_MASK (0xF << FPRF_SHIFT)

// FPSCR bits by name. The architecture numbers FPSCR bits from the most
// significant end, hence the 31 - n.
enum FPSCRExceptionFlag : u32
{
  FPSCR_FX = 1U << (31 - 0),
  FPSCR_FEX = 1U << (31 - 1),
  FPSCR_VX = 1U << (31 - 2),
  FPSCR_OX = 1U << (31 - 3),
  FPSCR_UX = 1U << (31 - 4),
  FPSCR_ZX = 1U << (31 - 5),
  FPSCR_XX = 1U << (31 - 6),
  FPSCR_VXSNAN = 1U << (31 - 7),
  FPSCR_VXISI = 1U << (31 - 8),
  FPSCR_VXIDI = 1U << (31 - 9),
  FPSCR_VXZDZ = 1U << (31 - 10),
  FPSCR_VXIMZ = 1U << (31 - 11),
  FPSCR_VXVC = 1U << (31 - 12),
  FPSCR_VXSOFT = 1U << (31 - 21),
  FPSCR_VXSQRT = 1U << (31 - 22),
  FPSCR_VXCVI = 1U << (31 - 23),
  FPSCR_VE = 1U << (31 - 24),
  FPSCR_OE = 1U << (31 - 25),
  FPSCR_UE = 1U << (31 - 26),
  FPSCR_ZE = 1U << (31 - 27),
  FPSCR_XE = 1U << (31 - 28),

  // Every invalid-operation cause.
  FPSCR_VX_ANY = FPSCR_VXSNAN | FPSCR_VXISI | FPSCR_VXIDI | FPSCR_VXZDZ | FPSCR_VXIMZ | FPSCR_VXVC |
                 FPSCR_VXSOFT | FPSCR_VXSQRT | FPSCR_VXCVI,
  // Every exception bit that feeds FX.
  FPSCR_ANY_X = FPSCR_OX | FPSCR_UX | FPSCR_ZX | FPSCR_XX | FPSCR_VX_ANY,
  // Every exception enable bit.
  FPSCR_ANY_E = FPSCR_VE | FPSCR_OE | FPSCR_UE | FPSCR_ZE | FPSCR_XE,
};

// Floating-point status and control register.
union UReg_FPSCR
{
  BitField<0, 2, Common::FPU::RoundMode> RN;  // rounding mode
  BitField<2, 1, u32> NI;                     // non-IEEE (flush denormals)
  BitField<3, 1, u32> XE;                     // enables: inexact,
  BitField<4, 1, u32> ZE;                     //   divide by zero,
  BitField<5, 1, u32> UE;                     //   underflow,
  BitField<6, 1, u32> OE;                     //   overflow,
  BitField<7, 1, u32> VE;                     //   invalid operation
  BitField<8, 1, u32> VXCVI;                  // sticky invalid-op causes:
  BitField<9, 1, u32> VXSQRT;                 //   integer convert, sqrt,
  BitField<10, 1, u32> VXSOFT;                //   software request
  BitField<11, 1, u32> reserved;
  BitField<12, 5, u32> FPRF;  // result class and FPCC (not sticky)
  BitField<17, 1, u32> FI;    // fraction inexact (not sticky)
  BitField<18, 1, u32> FR;    // fraction rounded (not sticky)
  BitField<19, 1, u32> VXVC;  // sticky invalid-op causes: compare,
  BitField<20, 1, u32> VXIMZ;   //   inf * 0,
  BitField<21, 1, u32> VXZDZ;   //   0 / 0,
  BitField<22, 1, u32> VXIDI;   //   inf / inf,
  BitField<23, 1, u32> VXISI;   //   inf - inf,
  BitField<24, 1, u32> VXSNAN;  //   signalling NaN
  BitField<25, 1, u32> XX;      // sticky inexact
  BitField<26, 1, u32> ZX;      // sticky divide by zero
  BitField<27, 1, u32> UX;      // sticky underflow
  BitField<28, 1, u32> OX;      // sticky overflow
  BitField<29, 1, u32> VX;      // any invalid-op cause (summary)
  BitField<30, 1, u32> FEX;     // any enabled exception (summary)
  BitField<31, 1, u32> FX;      // sticky "an exception bit went 0 -> 1"

  u32 Hex = 0;

  // Bit 11 (big-endian bit 20) is reserved: the hardware keeps it zero and
  // ignores writes to it.
  static constexpr u32 mask = ~(u32{1} << 11);

  UReg_FPSCR() = default;
  explicit UReg_FPSCR(u32 hex_) : Hex{hex_ & mask} {}

  UReg_FPSCR& operator=(u32 value)
  {
    Hex = value & mask;
    return *this;
  }

  UReg_FPSCR& operator|=(u32 value)
  {
    Hex |= value & mask;
    return *this;
  }

  // Clearing can never set the reserved bit, so no mask is needed.
  UReg_FPSCR& operator&=(u32 value)
  {
    Hex &= value;
    return *this;
  }

  UReg_FPSCR& operator^=(u32 value)
  {
    Hex ^= value & mask;
    return *this;
  }

  void ClearFIFR()
  {
    FI = 0;
    FR = 0;
  }
};

// HID0: cache, power-saving and bus configuration.
union UReg_HID0
{
  BitField<0, 1, u32> NOOPTI;
  BitField<1, 1, u32> reserved_1;
  BitField<2, 1, u32> BHT;
  BitField<3, 1, u32> ABE;
  BitField<4, 1, u32> reserved_2;
  BitField<5, 1, u32> BTIC;
  BitField<6, 1, u32> DCFA;
  BitField<7, 1, u32> SGE;
  BitField<8, 1, u32> IFEM;
  BitField<9, 1, u32> SPD;
  BitField<10, 1, u32> DCFI;  // data cache flash invalidate
  BitField<11, 1, u32> ICFI;  // instruction cache flash invalidate
  BitField<12, 1, u32> DLOCK;
  BitField<13, 1, u32> ILOCK;
  BitField<14, 1, u32> DCE;  // data cache enable
  BitField<15, 1, u32> ICE;  // instruction cache enable
  BitField<16, 1, u32> NHR;
  BitField<17, 3, u32> reserved_3;
  BitField<20, 1, u32> DPM;
  BitField<21, 1, u32> SLEEP;
  BitField<22, 1, u32> NAP;
  BitField<23, 1, u32> DOZE;
  BitField<24, 1, u32> PAR;
  BitField<25, 1, u32> ECLK;
  BitField<26, 1, u32> reserved_4;
  BitField<27, 1, u32> BCLK;
  BitField<28, 1, u32> EBD;
  BitField<29, 1, u32> EBA;
  BitField<30, 1, u32> DBP;
  BitField<31, 1, u32> EMCP;

  u32 Hex = 0;
};

// HID2: Gekko extensions (paired singles, locked cache, write gather pipe,
// load/store quantized) and DMA queue error reporting.
union UReg_HID2
{
  BitField<0, 16, u32> reserved;
  BitField<16, 1, u32> DQOEE;
  BitField<17, 1, u32> DCMEE;
  BitField<18, 1, u32> DNCEE;
  BitField<19, 1, u32> DCHEE;
  BitField<20, 1, u32> DQOERR;
  BitField<21, 1, u32> DCMERR;
  BitField<22, 1, u32> DNCERR;
  BitField<23, 1, u32> DCHERR;
  BitField<24, 4, u32> DMAQL;  // DMA queue length
  BitField<28, 1, u32> LCE;    // locked cache enable
  BitField<29, 1, u32> PSE;    // paired singles enable
  BitField<30, 1, u32> WPE;    // write gather pipe enable
  BitField<31, 1, u32> LSQE;   // load/store quantized enable

  u32 Hex = 0;

  UReg_HID2() = default;
  explicit UReg_HID2(u32 hex_) : Hex{hex_} {}
};

// HID4 (Broadway): L2 and bus settings; SBE enables the extra four BAT
// pairs.
union UReg_HID4
{
  BitField<0, 20, u32> reserved_1;
  BitField<20, 1, u32> L2CFI;
  BitField<21, 1, u32> L2MUM;
  BitField<22, 1, u32> DBP;
  BitField<23, 1, u32> LPE;
  BitField<24, 1, u32> ST0;
  BitField<25, 1, u32> SBE;
  BitField<26, 1, u32> reserved_2;
  BitField<27, 2, u32> BPD;
  BitField<29, 2, u32> L2FM;
  BitField<31, 1, u32> reserved_3;

  u32 Hex = 0;

  UReg_HID4() = default;
  explicit UReg_HID4(u32 hex_) : Hex{hex_} {}
};

// SDR1: physical base and size mask of the hashed page table.
union UReg_SDR1
{
  BitField<0, 9, u32> htabmask;
  BitField<9, 7, u32> reserved;
  BitField<16, 16, u32> htaborg;

  u32 Hex = 0;

  UReg_SDR1() = default;
  explicit UReg_SDR1(u32 hex_) : Hex{hex_} {}
};

// Performance monitor control registers.
union UReg_MMCR0
{
  BitField<0, 6, u32> PMC2SELECT;
  BitField<6, 7, u32> PMC1SELECT;
  BitField<13, 1, u32> PMCTRIGGER;
  BitField<14, 1, u32> PMCINTCONTROL;
  BitField<15, 1, u32> PMC1INTCONTROL;
  BitField<16, 6, u32> THRESHOLD;
  BitField<22, 1, u32> INTONBITTRANS;
  BitField<23, 2, u32> RTCSELECT;
  BitField<25, 1, u32> DISCOUNT;
  BitField<26, 1, u32> ENINT;
  BitField<27, 1, u32> DMR;
  BitField<28, 1, u32> DMS;
  BitField<29, 1, u32> DU;
  BitField<30, 1, u32> DP;
  BitField<31, 1, u32> DIS;

  u32 Hex = 0;
};

union UReg_MMCR1
{
  BitField<0, 22, u32> reserved;
  BitField<22, 5, u32> PMC4SELECT;
  BitField<27, 5, u32> PMC3SELECT;

  u32 Hex = 0;
};

// WPAR: write gather pipe target address; BNE = buffer not empty.
union UReg_WPAR
{
  BitField<0, 1, u32> BNE;
  BitField<1, 4, u32> reserved;
  BitField<5, 27, u32> GB_ADDR;

  u32 Hex = 0;

  UReg_WPAR() = default;
  explicit UReg_WPAR(u32 hex_) : Hex{hex_} {}
};

// Locked-cache DMA: DMAU holds the memory address and the upper length bits,
// DMAL the locked-cache address, the lower length bits, direction and the
// trigger/flush bits.
union UReg_DMAU
{
  BitField<0, 5, u32> DMA_LEN_U;
  BitField<5, 27, u32> MEM_ADDR;

  u32 Hex = 0;

  UReg_DMAU() = default;
  explicit UReg_DMAU(u32 hex_) : Hex{hex_} {}
};

union UReg_DMAL
{
  BitField<0, 1, u32> DMA_F;
  BitField<1, 1, u32> DMA_T;
  BitField<2, 2, u32> DMA_LEN_L;
  BitField<4, 1, u32> DMA_LD;
  BitField<5, 27, u32> LC_ADDR;

  u32 Hex = 0;

  UReg_DMAL() = default;
  explicit UReg_DMAL(u32 hex_) : Hex{hex_} {}
};

// Block address translation, upper word: effective block number, block
// length mask, and the supervisor/user valid bits.
union UReg_BAT_Up
{
  BitField<0, 1, u32> VP;
  BitField<1, 1, u32> VS;
  BitField<2, 11, u32> BL;
  BitField<13, 4, u32> reserved;
  BitField<17, 15, u32> BEPI;

  u32 Hex = 0;

  UReg_BAT_Up() = default;
  explicit UReg_BAT_Up(u32 hex_) : Hex{hex_} {}
};

// Block address translation, lower word: physical block number, WIMG and
// protection.
union UReg_BAT_Lo
{
  BitField<0, 2, u32> PP;
  BitField<2, 1, u32> reserved_1;
  BitField<3, 4, u32> WIMG;
  BitField<7, 10, u32> reserved_2;
  BitField<17, 15, u32> BRPN;

  u32 Hex = 0;

  UReg_BAT_Lo() = default;
  explicit UReg_BAT_Lo(u32 hex_) : Hex{hex_} {}
};

// Segment register. With T = 0 it names a virtual segment; with T = 1 it is
// a direct-store segment and BUID/CNTLR_SPEC apply instead.
union UReg_SR
{
  BitField<0, 24, u32> VSID;
  BitField<24, 4, u32> reserved;
  BitField<28, 1, u32> N;   // no-execute
  BitField<29, 1, u32> Kp;  // user-state key
  BitField<30, 1, u32> Ks;  // supervisor-state key
  BitField<31, 1, u32> T;

  BitField<0, 20, u32> CNTLR_SPEC;
  BitField<20, 9, u32> BUID;

  u32 Hex = 0;

  UReg_SR() = default;
  explicit UReg_SR(u32 hex_) : Hex{hex_} {}
};

// Thermal assist unit.
union UReg_THRM12
{
  BitField<0, 1, u32> V;
  BitField<1, 1, u32> TIE;
  BitField<2, 1, u32> TID;
  BitField<3, 20, u32> reserved;
  BitField<23, 7, u32> THRESHOLD;  // degrees Celsius
  BitField<30, 1, u32> TIV;
  BitField<31, 1, u32> TIN;

  u32 Hex = 0;

  UReg_THRM12() = default;
  explicit UReg_THRM12(u32 hex_) : Hex{hex_} {}
};

union UReg_THRM3
{
  BitField<0, 1, u32> E;
  BitField<1, 13, u32> SITV;
  BitField<14, 18, u32> reserved;

  u32 Hex = 0;

  UReg_THRM3() = default;
  explicit UReg_THRM3(u32 hex_) : Hex{hex_} {}
};

// Page table entry, first word: valid, VSID, hash function, abbreviated
// page index.
union UPTE_Lo
{
  BitField<0, 6, u32> API;
  BitField<6, 1, u32> H;
  BitField<7, 24, u32> VSID;
  BitField<31, 1, u32> V;

  u32 Hex = 0;

  UPTE_Lo() = default;
  explicit UPTE_Lo(u32 hex_) : Hex{hex_} {}
};

// Page table entry, second word: physical page number, referenced and
// changed bits, WIMG and protection.
union UPTE_Hi
{
  BitField<0, 2, u32> PP;
  BitField<2, 1, u32> reserved_1;
  BitField<3, 4, u32> WIMG;
  BitField<7, 1, u32> C;
  BitField<8, 1, u32> R;
  BitField<9, 3, u32> reserved_2;
  BitField<12, 20, u32> RPN;

  u32 Hex = 0;

  UPTE_Hi() = default;
  explicit UPTE_Hi(u32 hex_) : Hex{hex_} {}
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// BO field bits of the conditional branches.
enum
{
  BO_BRANCH_IF_CTR_0 = 2,
  BO_DONT_DECREMENT_FLAG = 4,
  BO_BRANCH_IF_TRUE = 8,
  BO_DONT_CHECK_CONDITION = 16,
};

// SPR numbers as mfspr/mtspr see them (halves already swapped back).
enum
{
  SPR_XER = 1,
  SPR_LR = 8,
  SPR_CTR = 9,
  SPR_DSISR = 18,
  SPR_DAR = 19,
  SPR_DEC = 22,
  SPR_SDR = 25,
  SPR_SRR0 = 26,
  SPR_SRR1 = 27,

  SPR_TL = 268,
  SPR_TU = 269,
  SPR_SPRG0 = 272,
  SPR_SPRG1 = 273,
  SPR_SPRG2 = 274,
  SPR_SPRG3 = 275,
  SPR_ASR = 280,
  SPR_EAR = 282,
  SPR_TL_W = 284,
  SPR_TU_W = 285,
  SPR_PVR = 287,

  SPR_IBAT0U = 528,
  SPR_IBAT0L = 529,
  SPR_IBAT1U = 530,
  SPR_IBAT1L = 531,
  SPR_IBAT2U = 532,
  SPR_IBAT2L = 533,
  SPR_IBAT3U = 534,
  SPR_IBAT3L = 535,
  SPR_DBAT0U = 536,
  SPR_DBAT0L = 537,
  SPR_DBAT1U = 538,
  SPR_DBAT1L = 539,
  SPR_DBAT2U = 540,
  SPR_DBAT2L = 541,
  SPR_DBAT3U = 542,
  SPR_DBAT3L = 543,
  SPR_IBAT4U = 560,
  SPR_IBAT4L = 561,
  SPR_IBAT5U = 562,
  SPR_IBAT5L = 563,
  SPR_IBAT6U = 564,
  SPR_IBAT6L = 565,
  SPR_IBAT7U = 566,
  SPR_IBAT7L = 567,
  SPR_DBAT4U = 568,
  SPR_DBAT4L = 569,
  SPR_DBAT5U = 570,
  SPR_DBAT5L = 571,
  SPR_DBAT6U = 572,
  SPR_DBAT6L = 573,
  SPR_DBAT7U = 574,
  SPR_DBAT7L = 575,

  SPR_GQR0 = 912,
  SPR_HID2 = 920,
  SPR_WPAR = 921,
  SPR_DMAU = 922,
  SPR_DMAL = 923,
  SPR_ECID_U = 924,
  SPR_ECID_M = 925,
  SPR_ECID_L = 926,

  SPR_UMMCR0 = 936,
  SPR_UPMC1 = 937,
  SPR_UPMC2 = 938,
  SPR_USIA = 939,
  SPR_UMMCR1 = 940,
  SPR_UPMC3 = 941,
  SPR_UPMC4 = 942,
  SPR_MMCR0 = 952,
  SPR_PMC1 = 953,
  SPR_PMC2 = 954,
  SPR_SIA = 955,
  SPR_MMCR1 = 956,
  SPR_PMC3 = 957,
  SPR_PMC4 = 958,

  SPR_HID0 = 1008,
  SPR_HID1 = 1009,
  SPR_IABR = 1010,
  SPR_HID4 = 1011,
  SPR_DABR = 1013,
  SPR_L2CR = 1017,
  SPR_ICTC = 1019,
  SPR_THRM1 = 1020,
  SPR_THRM2 = 1021,
  SPR_THRM3 = 1022,
};

// Pending-exception bits (PowerPCState::Exceptions). The values are part of
// the savestate and the JITs test them directly.
enum
{
  EXCEPTION_DECREMENTER = 0x00000001,
  EXCEPTION_SYSCALL = 0x00000002,
  EXCEPTION_EXTERNAL_INT = 0x00000004,
  EXCEPTION_DSI = 0x00000008,
  EXCEPTION_ISI = 0x00000010,
  EXCEPTION_ALIGNMENT = 0x00000020,
  EXCEPTION_FPU_UNAVAILABLE = 0x00000040,
  EXCEPTION_PROGRAM = 0x00000080,
  EXCEPTION_PERFORMANCE_MONITOR = 0x00000100,

  // Not a hardware exception: a memory breakpoint was hit and execution has
  // to stop at the current instruction.
  EXCEPTION_FAKE_MEMCHECK_HIT = 0x00000200,
};

// CPU state the JIT specialises compiled blocks on.
enum CPUEmuFeatureFlags : u32
{
  FEATURE_FLAG_MSR_DR = 1 << 0,
  FEATURE_FLAG_MSR_IR = 1 << 1,
  FEATURE_FLAG_PERFMON = 1 << 2,
  FEATURE_FLAG_END_OF_ENUMERATION,
};

constexpr s32 SignExt16(s16 x)
{
  return x;
}

// Sign-extends a 26-bit branch displacement (LI with its two zero bits
// appended). Bits above bit 25 are only touched when the sign bit is set.
constexpr s32 SignExt26(u32 x)
{
  constexpr u32 sign_bit = 1u << 25;
  constexpr u32 upper_bits = ~((sign_bit << 1) - 1);
  return static_cast<s32>((x & sign_bit) != 0 ? (x | upper_bits) : x);
}
