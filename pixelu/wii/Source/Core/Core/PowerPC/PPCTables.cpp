// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/PPCTables.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace PPCTables
{
namespace
{
// Where an instruction's encoding lives. The extended opcode (XO) sits in
// bits 21-30 (IBM numbering), i.e. SUBOP10. A-form instructions only decode
// the low five XO bits (the upper five hold frC), and the paired-single
// indexed loads/stores only decode six (the upper bits hold W and I).
enum class Form : u8
{
  Primary,  // OPCD alone
  Op4,      // OPCD 4, full XO
  Op4A,     // OPCD 4, XO bits 0-4 only
  Op4PSQ,   // OPCD 4, XO bits 0-5 only
  Op19,
  Op31,
  Op59,  // OPCD 59 decodes only five bits (SUBOP5)
  Op63,
  Op63A,
};

struct OpDef
{
  Form form;
  u16 xo;  // OPCD for Form::Primary
  const char* name;
  OpType type;
  u32 cycles;
  u64 flags;
};

// Common flag groups.
constexpr u64 LS = FL_LOADSTORE;
constexpr u64 FPU = FL_USE_FPU;
constexpr u64 PGM = FL_PROGRAMEXCEPTION;

// Integer loads and stores: displacement, displacement with update, indexed,
// and indexed with update.
constexpr u64 LOAD_D = FL_OUT_D | FL_IN_A0 | LS;
constexpr u64 LOAD_DU = FL_OUT_D | FL_OUT_A | FL_IN_A | LS;
constexpr u64 LOAD_X = FL_OUT_D | FL_IN_A0B | LS;
constexpr u64 LOAD_XU = FL_OUT_D | FL_OUT_A | FL_IN_AB | LS;
constexpr u64 STORE_D = FL_IN_A0 | FL_IN_S | LS;
constexpr u64 STORE_DU = FL_OUT_A | FL_IN_A | FL_IN_S | LS;
constexpr u64 STORE_X = FL_IN_S | FL_IN_A0B | LS;
constexpr u64 STORE_XU = FL_IN_S | FL_OUT_A | FL_IN_AB | LS;

// Floating-point loads and stores. Note that the non-indexed FP loads read
// rA as a register even when it is r0.
constexpr u64 FLOAD_S = FL_OUT_FLOAT_D | FPU | LS;
constexpr u64 FLOAD_D = FL_INOUT_FLOAT_D | FPU | LS;
constexpr u64 FSTORE = FL_IN_FLOAT_S | FPU | LS;

// Integer arithmetic (XO-form) with a register result and CR0 via Rc.
constexpr u64 ARITH_AB = FL_OUT_D | FL_IN_AB | FL_RC_BIT;
constexpr u64 ARITH_A = FL_OUT_D | FL_IN_A | FL_RC_BIT;
constexpr u64 CARRY_IN_OUT = FL_READ_CA | FL_SET_CA;
constexpr u64 OE = FL_SET_OE;

// Logical/shift ops writing rA from rS (and rB).
constexpr u64 LOGIC_SB = FL_OUT_A | FL_IN_SB | FL_RC_BIT;
constexpr u64 LOGIC_S = FL_OUT_A | FL_IN_S | FL_RC_BIT;

// Floating-point arithmetic that rounds, sets FPRF and can trap.
constexpr u64 FARITH = FL_RC_BIT_F | FPU | FL_SET_FPRF | FL_FLOAT_EXCEPTION;
constexpr u64 SINGLE_ARITH = FL_OUT_FLOAT_D | FARITH;
constexpr u64 DOUBLE_ARITH = FL_INOUT_FLOAT_D | FARITH;
constexpr u64 PAIRED_ARITH = FL_OUT_FLOAT_D | FARITH | PGM;
constexpr u64 PAIRED_COMPARE = FL_IN_FLOAT_AB | FL_SET_CRn | FPU | FL_READ_FPRF | FL_SET_FPRF |
                               PGM | FL_FLOAT_EXCEPTION;
constexpr u64 PAIRED_MOVE_B =
    FL_OUT_FLOAT_D | FL_IN_FLOAT_B | FL_IN_FLOAT_B_BITEXACT | FL_RC_BIT_F | FPU | PGM;
constexpr u64 PAIRED_MERGE =
    FL_OUT_FLOAT_D | FL_IN_FLOAT_AB | FL_IN_FLOAT_AB_BITEXACT | FL_RC_BIT_F | FPU | PGM;
constexpr u64 DOUBLE_MOVE_B =
    FL_INOUT_FLOAT_D | FL_IN_FLOAT_B | FL_IN_FLOAT_B_BITEXACT | FL_RC_BIT_F | FPU;
// fcmp also reads and writes FPRF; that is a conservative description, since
// a later FPRF write would make computing it here unnecessary.
constexpr u64 FCOMPARE =
    FL_IN_FLOAT_AB | FL_SET_CRn | FPU | FL_READ_FPRF | FL_SET_FPRF | FL_FLOAT_EXCEPTION;
constexpr u64 FPSCR_WRITE = FL_RC_BIT_F | FPU | FL_READ_FPRF | FL_SET_FPRF;

constexpr u64 BRANCH_COND = FL_ENDBLOCK | FL_READ_CR_BI;

using enum Form;
using enum OpType;

// clang-format off
constexpr OpDef s_ops[] = {
    // --- Opcodes that select a secondary table ---------------------------
    {Primary,  4, "RunTable4",  Subtable, 0, 0},
    {Primary, 19, "RunTable19", Subtable, 0, 0},
    {Primary, 31, "RunTable31", Subtable, 0, 0},
    {Primary, 59, "RunTable59", Subtable, 0, 0},
    {Primary, 63, "RunTable63", Subtable, 0, 0},

    // --- Branches and flow control ---------------------------------------
    {Primary, 16, "bcx",    Branch, 1, BRANCH_COND},
    {Primary, 18, "bx",     Branch, 1, FL_ENDBLOCK},
    {Op19,    16, "bclrx",  Branch, 1, BRANCH_COND},
    {Op19,   528, "bcctrx", Branch, 1, BRANCH_COND},
    {Primary,  3, "twi",    System, 1, FL_IN_A | FL_ENDBLOCK},
    {Op31,     4, "tw",     System, 2, FL_IN_AB | FL_ENDBLOCK},
    {Primary, 17, "sc",     System, 2, FL_ENDBLOCK},
    {Op19,    50, "rfi",    System, 2, FL_ENDBLOCK | FL_CHECKEXCEPTIONS | PGM | FL_SET_MSR},

    // --- Condition register logic ----------------------------------------
    {Op19,  33, "crnor",  CR, 1, 0},
    {Op19, 129, "crandc", CR, 1, 0},
    {Op19, 193, "crxor",  CR, 1, 0},
    {Op19, 225, "crnand", CR, 1, 0},
    {Op19, 257, "crand",  CR, 1, 0},
    {Op19, 289, "creqv",  CR, 1, 0},
    {Op19, 417, "crorc",  CR, 1, 0},
    {Op19, 449, "cror",   CR, 1, 0},
    {Op19,   0, "mcrf",   System, 1, FL_SET_CRn | FL_READ_CRn},
    {Op31, 512, "mcrxr",  System, 1, FL_SET_CRn | CARRY_IN_OUT},
    {Op31,  19, "mfcr",   System, 1, FL_OUT_D | FL_READ_ALL_CR},
    {Op31, 144, "mtcrf",  System, 1, FL_IN_S | FL_SET_ALL_CR | FL_READ_ALL_CR},

    // --- Integer arithmetic with immediates ------------------------------
    {Primary,  7, "mulli",    Integer, 3, FL_OUT_D | FL_IN_A},
    {Primary,  8, "subfic",   Integer, 1, FL_OUT_D | FL_IN_A | FL_SET_CA},
    {Primary, 12, "addic",    Integer, 1, FL_OUT_D | FL_IN_A | FL_SET_CA},
    {Primary, 13, "addic_rc", Integer, 1, FL_OUT_D | FL_IN_A | FL_SET_CA | FL_SET_CR0},
    {Primary, 14, "addi",     Integer, 1, FL_OUT_D | FL_IN_A0},
    {Primary, 15, "addis",    Integer, 1, FL_OUT_D | FL_IN_A0},
    {Primary, 10, "cmpli",    Integer, 1, FL_IN_A | FL_SET_CRn},
    {Primary, 11, "cmpi",     Integer, 1, FL_IN_A | FL_SET_CRn},

    // --- Integer arithmetic, register forms (each with its OE=1 twin) ----
    {Op31, 266, "addx",     Integer, 1, ARITH_AB},
    {Op31, 778, "addox",    Integer, 1, ARITH_AB | OE},
    {Op31,  10, "addcx",    Integer, 1, ARITH_AB | FL_SET_CA},
    {Op31, 522, "addcox",   Integer, 1, ARITH_AB | FL_SET_CA | OE},
    {Op31, 138, "addex",    Integer, 1, ARITH_AB | CARRY_IN_OUT},
    {Op31, 650, "addeox",   Integer, 1, ARITH_AB | CARRY_IN_OUT | OE},
    {Op31, 234, "addmex",   Integer, 1, ARITH_A | CARRY_IN_OUT},
    {Op31, 746, "addmeox",  Integer, 1, ARITH_A | CARRY_IN_OUT | OE},
    {Op31, 202, "addzex",   Integer, 1, ARITH_A | CARRY_IN_OUT},
    {Op31, 714, "addzeox",  Integer, 1, ARITH_A | CARRY_IN_OUT | OE},
    {Op31,  40, "subfx",    Integer, 1, ARITH_AB},
    {Op31, 552, "subfox",   Integer, 1, ARITH_AB | OE},
    {Op31,   8, "subfcx",   Integer, 1, ARITH_AB | FL_SET_CA},
    {Op31, 520, "subfcox",  Integer, 1, ARITH_AB | FL_SET_CA | OE},
    {Op31, 136, "subfex",   Integer, 1, ARITH_AB | CARRY_IN_OUT},
    {Op31, 648, "subfeox",  Integer, 1, ARITH_AB | CARRY_IN_OUT | OE},
    {Op31, 232, "subfmex",  Integer, 1, ARITH_A | CARRY_IN_OUT},
    {Op31, 744, "subfmeox", Integer, 1, ARITH_A | CARRY_IN_OUT | OE},
    {Op31, 200, "subfzex",  Integer, 1, ARITH_A | CARRY_IN_OUT},
    {Op31, 712, "subfzeox", Integer, 1, ARITH_A | CARRY_IN_OUT | OE},
    {Op31, 104, "negx",     Integer, 1, ARITH_A},
    {Op31, 616, "negox",    Integer, 1, ARITH_A | OE},
    {Op31,  75, "mulhwx",   Integer, 5, ARITH_AB},
    {Op31,  11, "mulhwux",  Integer, 5, ARITH_AB},
    {Op31, 235, "mullwx",   Integer, 5, ARITH_AB},
    {Op31, 747, "mullwox",  Integer, 5, ARITH_AB | OE},
    {Op31, 491, "divwx",    Integer, 40, ARITH_AB},
    {Op31,1003, "divwox",   Integer, 40, ARITH_AB | OE},
    {Op31, 459, "divwux",   Integer, 40, ARITH_AB},
    {Op31, 971, "divwuox",  Integer, 40, ARITH_AB | OE},
    {Op31,   0, "cmp",      Integer, 1, FL_IN_AB | FL_SET_CRn},
    {Op31,  32, "cmpl",     Integer, 1, FL_IN_AB | FL_SET_CRn},

    // --- Logical, rotate and shift ---------------------------------------
    {Primary, 24, "ori",      Integer, 1, FL_OUT_A | FL_IN_S},
    {Primary, 25, "oris",     Integer, 1, FL_OUT_A | FL_IN_S},
    {Primary, 26, "xori",     Integer, 1, FL_OUT_A | FL_IN_S},
    {Primary, 27, "xoris",    Integer, 1, FL_OUT_A | FL_IN_S},
    {Primary, 28, "andi_rc",  Integer, 1, FL_OUT_A | FL_IN_S | FL_SET_CR0},
    {Primary, 29, "andis_rc", Integer, 1, FL_OUT_A | FL_IN_S | FL_SET_CR0},
    {Op31,  28, "andx",    Integer, 1, LOGIC_SB},
    {Op31,  60, "andcx",   Integer, 1, LOGIC_SB},
    {Op31, 124, "norx",    Integer, 1, LOGIC_SB},
    {Op31, 284, "eqvx",    Integer, 1, LOGIC_SB},
    {Op31, 316, "xorx",    Integer, 1, LOGIC_SB},
    {Op31, 412, "orcx",    Integer, 1, LOGIC_SB},
    {Op31, 444, "orx",     Integer, 1, LOGIC_SB},
    {Op31, 476, "nandx",   Integer, 1, LOGIC_SB},
    {Op31,  26, "cntlzwx", Integer, 1, LOGIC_S},
    {Op31, 922, "extshx",  Integer, 1, LOGIC_S},
    {Op31, 954, "extsbx",  Integer, 1, LOGIC_S},
    {Primary, 20, "rlwimix", Integer, 1, FL_OUT_A | FL_IN_A | FL_IN_S | FL_RC_BIT},
    {Primary, 21, "rlwinmx", Integer, 1, LOGIC_S},
    {Primary, 23, "rlwnmx",  Integer, 1, LOGIC_SB},
    {Op31,  24, "slwx",   Integer, 1, LOGIC_SB},
    {Op31, 536, "srwx",   Integer, 1, LOGIC_SB},
    {Op31, 792, "srawx",  Integer, 1, LOGIC_SB | FL_SET_CA},
    {Op31, 824, "srawix", Integer, 1, LOGIC_S | FL_SET_CA},

    // --- Integer loads ---------------------------------------------------
    {Primary, 32, "lwz",  Load, 1, LOAD_D},
    {Primary, 33, "lwzu", Load, 1, LOAD_DU},
    {Primary, 34, "lbz",  Load, 1, LOAD_D},
    {Primary, 35, "lbzu", Load, 1, LOAD_DU},
    {Primary, 40, "lhz",  Load, 1, LOAD_D},
    {Primary, 41, "lhzu", Load, 1, LOAD_DU},
    {Primary, 42, "lha",  Load, 1, LOAD_D},
    {Primary, 43, "lhau", Load, 1, LOAD_DU},
    {Op31,  23, "lwzx",  Load, 1, LOAD_X},
    {Op31,  55, "lwzux", Load, 1, LOAD_XU},
    {Op31,  87, "lbzx",  Load, 1, LOAD_X},
    {Op31, 119, "lbzux", Load, 1, LOAD_XU},
    {Op31, 279, "lhzx",  Load, 1, LOAD_X},
    {Op31, 311, "lhzux", Load, 1, LOAD_XU},
    {Op31, 343, "lhax",  Load, 1, LOAD_X},
    {Op31, 375, "lhaux", Load, 1, LOAD_XU},
    {Op31, 534, "lwbrx", Load, 1, LOAD_X},
    {Op31, 790, "lhbrx", Load, 1, LOAD_X},
    {Op31,  20, "lwarx", Load, 1, LOAD_X | FL_SET_CR0},
    {Op31, 533, "lswx",  Load, 1, LOAD_X},
    {Op31, 597, "lswi",  Load, 1, LOAD_D},
    {Primary, 46, "lmw", System, 11, FL_IN_A0 | LS},

    // --- Integer stores --------------------------------------------------
    {Primary, 36, "stw",  Store, 1, STORE_D},
    {Primary, 37, "stwu", Store, 1, STORE_DU},
    {Primary, 38, "stb",  Store, 1, STORE_D},
    {Primary, 39, "stbu", Store, 1, STORE_DU},
    {Primary, 44, "sth",  Store, 1, STORE_D},
    {Primary, 45, "sthu", Store, 1, STORE_DU},
    {Op31, 151, "stwx",   Store, 1, STORE_X},
    {Op31, 183, "stwux",  Store, 1, STORE_XU},
    {Op31, 215, "stbx",   Store, 1, STORE_X},
    {Op31, 247, "stbux",  Store, 1, STORE_XU},
    {Op31, 407, "sthx",   Store, 1, STORE_X},
    {Op31, 439, "sthux",  Store, 1, STORE_XU},
    {Op31, 662, "stwbrx", Store, 1, STORE_X},
    {Op31, 918, "sthbrx", Store, 1, STORE_X},
    {Op31, 150, "stwcxd", Store, 1, STORE_X | FL_SET_CR0},
    {Op31, 661, "stswx",  Store, 1, FL_IN_A0B | LS},
    {Op31, 725, "stswi",  Store, 1, FL_IN_A0 | LS},
    {Primary, 47, "stmw", System, 11, FL_IN_A0 | LS},

    // --- Floating-point loads and stores ---------------------------------
    {Primary, 48, "lfs",   LoadFP,  1, FLOAD_S | FL_IN_A},
    {Primary, 49, "lfsu",  LoadFP,  1, FLOAD_S | FL_OUT_A | FL_IN_A},
    {Primary, 50, "lfd",   LoadFP,  1, FLOAD_D | FL_IN_A},
    {Primary, 51, "lfdu",  LoadFP,  1, FLOAD_D | FL_OUT_A | FL_IN_A},
    {Primary, 52, "stfs",  StoreFP, 1, FSTORE | FL_IN_A0},
    {Primary, 53, "stfsu", StoreFP, 1, FSTORE | FL_OUT_A | FL_IN_A},
    {Primary, 54, "stfd",  StoreFP, 1, FSTORE | FL_IN_A0},
    {Primary, 55, "stfdu", StoreFP, 1, FSTORE | FL_OUT_A | FL_IN_A},
    {Op31, 535, "lfsx",   LoadFP,  1, FLOAD_S | FL_IN_A0B},
    {Op31, 567, "lfsux",  LoadFP,  1, FLOAD_S | FL_IN_AB | FL_OUT_A},
    {Op31, 599, "lfdx",   LoadFP,  1, FLOAD_D | FL_IN_A0B},
    {Op31, 631, "lfdux",  LoadFP,  1, FLOAD_D | FL_IN_AB | FL_OUT_A},
    {Op31, 663, "stfsx",  StoreFP, 1, FSTORE | FL_IN_A0B},
    {Op31, 695, "stfsux", StoreFP, 1, FSTORE | FL_IN_AB | FL_OUT_A},
    {Op31, 727, "stfdx",  StoreFP, 1, FSTORE | FL_IN_A0B},
    {Op31, 759, "stfdux", StoreFP, 1, FSTORE | FL_IN_AB | FL_OUT_A},
    {Op31, 983, "stfiwx", StoreFP, 1, FSTORE | FL_IN_A0B},

    // --- Paired-single (quantized) loads and stores ----------------------
    {Primary, 56, "psq_l",   LoadPS,  1, FL_OUT_FLOAT_D | FL_IN_A0 | FPU | LS | PGM},
    {Primary, 57, "psq_lu",  LoadPS,  1, FL_OUT_FLOAT_D | FL_OUT_A | FL_IN_A | FPU | LS | PGM},
    {Primary, 60, "psq_st",  StorePS, 1, FSTORE | FL_IN_A0 | PGM},
    {Primary, 61, "psq_stu", StorePS, 1, FSTORE | FL_OUT_A | FL_IN_A | PGM},
    {Op4PSQ,  6, "psq_lx",   LoadPS,  1, FL_OUT_FLOAT_D | FL_IN_A0B | FPU | LS},
    {Op4PSQ,  7, "psq_stx",  StorePS, 1, FSTORE | FL_IN_A0B},
    {Op4PSQ, 38, "psq_lux",  LoadPS,  1, FL_OUT_FLOAT_D | FL_OUT_A | FL_IN_AB | FPU | LS},
    {Op4PSQ, 39, "psq_stux", StorePS, 1, FSTORE | FL_OUT_A | FL_IN_AB},

    // --- Paired-single arithmetic ----------------------------------------
    {Op4A, 10, "ps_sum0",   PS, 1, PAIRED_ARITH | FL_IN_FLOAT_ABC},
    {Op4A, 11, "ps_sum1",   PS, 1, PAIRED_ARITH | FL_IN_FLOAT_ABC},
    {Op4A, 12, "ps_muls0",  PS, 1, PAIRED_ARITH | FL_IN_FLOAT_AC},
    {Op4A, 13, "ps_muls1",  PS, 1, PAIRED_ARITH | FL_IN_FLOAT_AC},
    {Op4A, 14, "ps_madds0", PS, 1, PAIRED_ARITH | FL_IN_FLOAT_ABC},
    {Op4A, 15, "ps_madds1", PS, 1, PAIRED_ARITH | FL_IN_FLOAT_ABC},
    {Op4A, 18, "ps_div",    PS, 17, PAIRED_ARITH | FL_IN_FLOAT_AB | FL_FLOAT_DIV},
    {Op4A, 20, "ps_sub",    PS, 1, PAIRED_ARITH | FL_IN_FLOAT_AB},
    {Op4A, 21, "ps_add",    PS, 1, PAIRED_ARITH | FL_IN_FLOAT_AB},
    {Op4A, 23, "ps_sel",    PS, 1, FL_OUT_FLOAT_D | FL_IN_FLOAT_ABC | FL_IN_FLOAT_BC_BITEXACT | FL_RC_BIT_F | FPU | PGM},
    {Op4A, 24, "ps_res",    PS, 1, PAIRED_ARITH | FL_IN_FLOAT_B | FL_FLOAT_DIV},
    {Op4A, 25, "ps_mul",    PS, 1, PAIRED_ARITH | FL_IN_FLOAT_AC},
    {Op4A, 26, "ps_rsqrte", PS, 2, PAIRED_ARITH | FL_IN_FLOAT_B | FL_FLOAT_DIV},
    {Op4A, 28, "ps_msub",   PS, 1, PAIRED_ARITH | FL_IN_FLOAT_ABC},
    {Op4A, 29, "ps_madd",   PS, 1, PAIRED_ARITH | FL_IN_FLOAT_ABC},
    {Op4A, 30, "ps_nmsub",  PS, 1, PAIRED_ARITH | FL_IN_FLOAT_ABC},
    {Op4A, 31, "ps_nmadd",  PS, 1, PAIRED_ARITH | FL_IN_FLOAT_ABC},
    {Op4,   0, "ps_cmpu0",   PS, 1, PAIRED_COMPARE},
    {Op4,  32, "ps_cmpo0",   PS, 1, PAIRED_COMPARE},
    {Op4,  64, "ps_cmpu1",   PS, 1, PAIRED_COMPARE},
    {Op4,  96, "ps_cmpo1",   PS, 1, PAIRED_COMPARE},
    {Op4,  40, "ps_neg",     PS, 1, PAIRED_MOVE_B},
    {Op4,  72, "ps_mr",      PS, 1, PAIRED_MOVE_B},
    {Op4, 136, "ps_nabs",    PS, 1, PAIRED_MOVE_B},
    {Op4, 264, "ps_abs",     PS, 1, PAIRED_MOVE_B},
    {Op4, 528, "ps_merge00", PS, 1, PAIRED_MERGE},
    {Op4, 560, "ps_merge01", PS, 1, PAIRED_MERGE},
    {Op4, 592, "ps_merge10", PS, 1, PAIRED_MERGE},
    {Op4, 624, "ps_merge11", PS, 1, PAIRED_MERGE},

    // --- Single-precision arithmetic -------------------------------------
    {Op59, 18, "fdivsx",   SingleFP, 17, SINGLE_ARITH | FL_IN_FLOAT_AB | FL_FLOAT_DIV},
    {Op59, 20, "fsubsx",   SingleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_AB},
    {Op59, 21, "faddsx",   SingleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_AB},
    {Op59, 24, "fresx",    SingleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_B | FL_FLOAT_DIV},
    {Op59, 25, "fmulsx",   SingleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_AC},
    {Op59, 28, "fmsubsx",  SingleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_ABC},
    {Op59, 29, "fmaddsx",  SingleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_ABC},
    {Op59, 30, "fnmsubsx", SingleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_ABC},
    {Op59, 31, "fnmaddsx", SingleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_ABC},

    // --- Double-precision arithmetic, moves and conversions --------------
    {Op63A, 18, "fdivx",    DoubleFP, 31, DOUBLE_ARITH | FL_IN_FLOAT_AB | FL_FLOAT_DIV},
    {Op63A, 20, "fsubx",    DoubleFP, 1, DOUBLE_ARITH | FL_IN_FLOAT_AB},
    {Op63A, 21, "faddx",    DoubleFP, 1, DOUBLE_ARITH | FL_IN_FLOAT_AB},
    {Op63A, 23, "fselx",    DoubleFP, 1, FL_INOUT_FLOAT_D | FL_IN_FLOAT_ABC | FL_IN_FLOAT_BC_BITEXACT | FL_RC_BIT_F | FPU},
    {Op63A, 25, "fmulx",    DoubleFP, 1, DOUBLE_ARITH | FL_IN_FLOAT_AC},
    {Op63A, 26, "frsqrtex", DoubleFP, 1, DOUBLE_ARITH | FL_IN_FLOAT_B | FL_FLOAT_DIV},
    {Op63A, 28, "fmsubx",   DoubleFP, 1, DOUBLE_ARITH | FL_IN_FLOAT_ABC},
    {Op63A, 29, "fmaddx",   DoubleFP, 1, DOUBLE_ARITH | FL_IN_FLOAT_ABC},
    {Op63A, 30, "fnmsubx",  DoubleFP, 1, DOUBLE_ARITH | FL_IN_FLOAT_ABC},
    {Op63A, 31, "fnmaddx",  DoubleFP, 1, DOUBLE_ARITH | FL_IN_FLOAT_ABC},
    {Op63,   0, "fcmpu",   DoubleFP, 1, FCOMPARE},
    {Op63,  32, "fcmpo",   DoubleFP, 1, FCOMPARE},
    {Op63,  40, "fnegx",   DoubleFP, 1, DOUBLE_MOVE_B},
    {Op63,  72, "fmrx",    DoubleFP, 1, DOUBLE_MOVE_B},
    {Op63, 136, "fnabsx",  DoubleFP, 1, DOUBLE_MOVE_B},
    {Op63, 264, "fabsx",   DoubleFP, 1, DOUBLE_MOVE_B},
    {Op63,  12, "frspx",   DoubleFP, 1, SINGLE_ARITH | FL_IN_FLOAT_B},
    {Op63,  14, "fctiwx",  DoubleFP, 1, FL_INOUT_FLOAT_D | FL_IN_FLOAT_B | FL_RC_BIT_F | FPU | FL_FLOAT_EXCEPTION},
    {Op63,  15, "fctiwzx", DoubleFP, 1, FL_INOUT_FLOAT_D | FL_IN_FLOAT_B | FL_RC_BIT_F | FPU | FL_FLOAT_EXCEPTION},

    // --- FPSCR access ----------------------------------------------------
    {Op63,  64, "mcrfs",   SystemFP, 1, FL_SET_CRn | FPU | FL_READ_FPRF},
    {Op63, 583, "mffsx",   SystemFP, 1, FL_RC_BIT_F | FL_INOUT_FLOAT_D | FPU | FL_READ_FPRF},
    {Op63,  70, "mtfsb0x", SystemFP, 3, FPSCR_WRITE},
    {Op63,  38, "mtfsb1x", SystemFP, 3, FPSCR_WRITE | FL_FLOAT_EXCEPTION},
    {Op63, 134, "mtfsfix", SystemFP, 3, FPSCR_WRITE | FL_FLOAT_EXCEPTION},
    {Op63, 711, "mtfsfx",  SystemFP, 3, FPSCR_WRITE | FL_IN_FLOAT_B | FL_FLOAT_EXCEPTION},

    // --- Supervisor state and special registers --------------------------
    {Op31,  83, "mfmsr",  System, 1, FL_OUT_D | PGM},
    {Op31, 146, "mtmsr",  System, 1, FL_IN_S | FL_ENDBLOCK | PGM | FL_FLOAT_EXCEPTION | FL_SET_MSR},
    {Op31, 339, "mfspr",  SPR, 1, FL_OUT_D | PGM},
    {Op31, 467, "mtspr",  SPR, 2, FL_IN_S | FL_ENDBLOCK | PGM},
    {Op31, 371, "mftb",   System, 1, FL_OUT_D | FL_TIMER | PGM},
    {Op31, 595, "mfsr",   System, 3, FL_OUT_D | PGM},
    {Op31, 659, "mfsrin", System, 3, FL_OUT_D | FL_IN_B | PGM},
    {Op31, 210, "mtsr",   System, 1, FL_IN_S | PGM},
    {Op31, 242, "mtsrin", System, 1, FL_IN_SB | PGM},

    // --- Cache, TLB and synchronisation ----------------------------------
    {Op31,   54, "dcbst",  DataCache, 5, FL_IN_A0B | LS},
    {Op31,   86, "dcbf",   DataCache, 5, FL_IN_A0B | LS},
    {Op31,  246, "dcbtst", DataCache, 2, 0},
    {Op31,  278, "dcbt",   DataCache, 2, 0},
    {Op31,  470, "dcbi",   DataCache, 5, FL_IN_A0B | LS | PGM},
    {Op31,  758, "dcba",   DataCache, 5, 0},
    {Op31, 1014, "dcbz",   DataCache, 5, FL_IN_A0B | LS},
    {Op4,  1014, "dcbz_l", System, 1, FL_IN_A0B | LS | PGM},
    {Op31,  982, "icbi",   System, 4, FL_IN_A0B | FL_ENDBLOCK | LS},
    {Op19,  150, "isync",  InstructionCache, 1, FL_NO_REORDER},
    {Op31,  598, "sync",   System, 3, 0},
    {Op31,  854, "eieio",  System, 1, 0},
    {Op31,  306, "tlbie",  System, 1, FL_IN_B | PGM},
    {Op31,  566, "tlbsync", System, 1, PGM},
    // External control (not used by GameCube/Wii software).
    {Op31,  310, "eciwx",  System, 1, FL_IN_A0B | FL_OUT_D | LS},
    {Op31,  438, "ecowx",  System, 1, FL_IN_A0B | FL_IN_S | LS},
};
// clang-format on

// Slot 0 of the info array is the fallback for unrecognised encodings.
constexpr size_t OP_COUNT = std::size(s_ops) + 1;
constexpr u16 UNKNOWN = 0;

struct Tables
{
  std::array<GekkoOPInfo, OP_COUNT> info{};
  std::array<u16, 64> primary{};
  std::array<u16, 1024> op4{};
  std::array<u16, 1024> op19{};
  std::array<u16, 1024> op31{};
  std::array<u16, 32> op59{};
  std::array<u16, 1024> op63{};
};

std::array<GekkoOPStats, OP_COUNT> s_stats;

// Deliberately not constexpr: reaching it while building the tables (two
// definitions for one encoding) makes the build fail.
void ConflictingEncoding()
{
}

consteval Tables BuildTables()
{
  Tables t{};
  t.info[UNKNOWN] = {"unknown_instruction", OpType::Unknown, 0, FL_ENDBLOCK, &s_stats[UNKNOWN]};

  const auto claim = [](auto& table, u32 index, u16 id) {
    if (table[index] != UNKNOWN)
      ConflictingEncoding();
    table[index] = id;
  };

  for (size_t i = 0; i < std::size(s_ops); ++i)
  {
    const OpDef& op = s_ops[i];
    const u16 id = static_cast<u16>(i + 1);
    t.info[id] = {op.name, op.type, op.cycles, op.flags, &s_stats[id]};

    switch (op.form)
    {
    case Form::Primary:
      claim(t.primary, op.xo, id);
      break;
    case Form::Op4:
      claim(t.op4, op.xo, id);
      break;
    case Form::Op4A:
      for (u32 high = 0; high < 1024; high += 32)
        claim(t.op4, high | op.xo, id);
      break;
    case Form::Op4PSQ:
      for (u32 high = 0; high < 1024; high += 64)
        claim(t.op4, high | op.xo, id);
      break;
    case Form::Op19:
      claim(t.op19, op.xo, id);
      break;
    case Form::Op31:
      claim(t.op31, op.xo, id);
      break;
    case Form::Op59:
      claim(t.op59, op.xo, id);
      break;
    case Form::Op63:
      claim(t.op63, op.xo, id);
      break;
    case Form::Op63A:
      for (u32 high = 0; high < 1024; high += 32)
        claim(t.op63, high | op.xo, id);
      break;
    }
  }
  return t;
}

constexpr Tables s_tables = BuildTables();

const GekkoOPInfo* InfoFor(u16 id)
{
  return &s_tables.info[id];
}
}  // namespace

const GekkoOPInfo* GetOpInfo(UGeckoInstruction inst, u32 pc)
{
  switch (inst.OPCD)
  {
  case 4:
    return InfoFor(s_tables.op4[inst.SUBOP10]);
  case 19:
    return InfoFor(s_tables.op19[inst.SUBOP10]);
  case 31:
    return InfoFor(s_tables.op31[inst.SUBOP10]);
  case 59:
    return InfoFor(s_tables.op59[inst.SUBOP5]);
  case 63:
    return InfoFor(s_tables.op63[inst.SUBOP10]);
  default:
    return InfoFor(s_tables.primary[inst.OPCD]);
  }
}

const char* GetInstructionName(UGeckoInstruction inst, u32 pc)
{
  return GetOpInfo(inst, pc)->opname;
}

bool IsValidInstruction(UGeckoInstruction inst, u32 pc)
{
  const OpType type = GetOpInfo(inst, pc)->type;
  return type != OpType::Invalid && type != OpType::Unknown;
}

void CountInstruction(UGeckoInstruction inst, u32 pc)
{
  ++GetOpInfo(inst, pc)->stats->run_count;
}

void CountInstructionCompile(const GekkoOPInfo* info, u32 pc)
{
  GekkoOPStats& stats = *info->stats;
  ++stats.compile_count;
  stats.last_use = pc;
}

void PrintInstructionRunCounts()
{
  std::array<std::pair<u64, const char*>, OP_COUNT> counts;
  for (size_t i = 0; i < OP_COUNT; ++i)
    counts[i] = {s_tables.info[i].stats->run_count, s_tables.info[i].opname};

  std::ranges::sort(counts, [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& [count, name] : counts)
  {
    if (count == 0)
      break;
    INFO_LOG_FMT(POWERPC, "{} : {}", name, count);
  }
}

void LogCompiledInstructions()
{
  // Each call writes a fresh numbered pair of files.
  static u32 s_dump_index = 0;
  const std::string dir = File::GetUserPath(D_LOGS_IDX);

  File::IOFile compiled(fmt::format("{}inst_log{}.txt", dir, s_dump_index), "w");
  for (const GekkoOPInfo& info : s_tables.info)
  {
    const GekkoOPStats& stats = *info.stats;
    if (stats.compile_count != 0)
    {
      compiled.WriteString(fmt::format("{}\t{}\t{}\t{:08x}\n", info.opname, stats.compile_count,
                                       stats.run_count, stats.last_use));
    }
  }

  File::IOFile never_compiled(fmt::format("{}inst_not{}.txt", dir, s_dump_index), "w");
  for (const GekkoOPInfo& info : s_tables.info)
  {
    const GekkoOPStats& stats = *info.stats;
    if (stats.compile_count == 0)
    {
      never_compiled.WriteString(
          fmt::format("{}\t{}\t{}\n", info.opname, stats.compile_count, stats.run_count));
    }
  }

  ++s_dump_index;
}

}  // namespace PPCTables
