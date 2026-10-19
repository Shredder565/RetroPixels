// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Intermediate code of the PowerPC recompiler.
//
// The front end turns guest instructions into a small register based
// instruction set operating on virtual registers. Virtual registers are
// either one of the fixed guest registers (GPRs, FPRs, CR bits, XER flags,
// LR, CTR, GQRs and the lwarx reservation) or temporaries created by the
// front end. Integer registers hold 32-bit values, float registers hold an
// Espresso FPR (two doubles: ps0 in the low lane, ps1 in the high lane).
//
// Guest registers have a home slot in PPCInterpreter_t. Generated code keeps
// them in host registers while they are in use and writes them back to the
// home slot when they stop being used, at function exits and before calls
// into the interpreter.

using IMLReg = uint16;
constexpr IMLReg IML_NOREG = 0xFFFF;

enum class IMLRegKind : uint8
{
	Int,   // 32-bit integer (CR bits and XER flags are 0 or 1)
	Float, // FPR pair, two doubles
};

// fixed virtual register numbers of the guest registers
namespace IMLGuest
{
	constexpr IMLReg GPR0 = 0;
	constexpr IMLReg FPR0 = 32;
	constexpr IMLReg CR0 = 64;      // one per CR bit, cr0.LT first
	constexpr IMLReg XER_CA = 96;
	constexpr IMLReg XER_OV = 97;
	constexpr IMLReg XER_SO = 98;
	constexpr IMLReg LR = 99;
	constexpr IMLReg CTR = 100;
	constexpr IMLReg GQR0 = 101;    // UGQR0-7
	constexpr IMLReg RESV_ADDR = 109;
	constexpr IMLReg RESV_VALUE = 110;
	constexpr IMLReg COUNT = 111;

	inline IMLReg GPR(uint32 i) { return (IMLReg)(GPR0 + i); }
	inline IMLReg FPR(uint32 i) { return (IMLReg)(FPR0 + i); }
	inline IMLReg CR(uint32 bit) { return (IMLReg)(CR0 + bit); }
	inline IMLReg GQR(uint32 i) { return (IMLReg)(GQR0 + i); }
	inline bool IsGuest(IMLReg r) { return r < COUNT; }
}

// conditions for compares and conditional branches
enum class IMLCond : uint8
{
	EQ,
	NE,
	SLT,
	SGT,
	SLE,
	SGE,
	ULT,
	UGT,
	ULE,
	UGE,
};

IMLCond IMLCond_Invert(IMLCond c);

enum class IMLOp : uint8
{
	NOP,

	// ---- integer ----
	MOV,        // d = a
	MOVI,       // d = imm
	ADD,        // d = a + b
	SUB,        // d = a - b
	AND,
	OR,
	XOR,
	ANDC,       // d = a & ~b
	ORC,        // d = a | ~b
	NOT,        // d = ~a
	NEG,        // d = -a
	MUL,        // d = low 32 bits of a * b
	MULHS,      // d = high 32 bits of signed a * b
	MULHU,      // d = high 32 bits of unsigned a * b
	DIVS,       // divw (Espresso results for x/0 and 0x80000000/-1)
	DIVU,       // divwu (x/0 = 0)
	SHL,        // d = a << imm (imm 0-31)
	SHR,        // d = a >> imm, logical
	SAR,        // d = a >> imm, arithmetic
	ROTL,       // d = rotl(a, b & 31) or rotl(a, imm)
	SLW,        // d = a << (b & 63), 0 when the amount is 32 or more
	SRW,        // d = a >> (b & 63), 0 when the amount is 32 or more
	SRAW,       // r0 = a >> (b & 63) arithmetic, r1 = CA (sraw/srawi semantics)
	EXTS8,
	EXTS16,
	CNTLZ,
	BSWAP32,
	BSWAP16,    // d = byte swapped low 16 bits of a, zero extended
	ADDC,       // r0 = a + b, r1 = carry out. INV_A complements a first
	ADDE,       // r0 = a + b + r1, r1 = carry out. INV_A complements a first
	CMPSET,     // d = cond(a, b) ? 1 : 0
	CMPCR,      // r0/r1/r2 = a <, >, == b (signed or unsigned per cond SLT/ULT)

	// ---- memory: address = r1 (base, may be IML_NOREG) + r2 (index, optional) + imm ----
	LOAD,       // r0 = load size bytes (big endian unless BYTEREV), SIGNEXT
	STORE,      // store size bytes of r0
	LOADFS,     // lfs: r0 both lanes = widened single
	LOADFD,     // lfd: r0.ps0 = double, ps1 kept
	STOREF,     // FP store of r0, sub = IMLFMem
	PSQLOAD,    // r0 = quantized pair, r3 = GQR value, PSQ_W flag
	PSQSTORE,   // store quantized r0, r3 = GQR value, PSQ_W flag
	ZEROLINE,   // zero the 32 byte line containing the address
	CMPXCHG,    // if r4 != 0: r0 = 1 if [r1] still held r2 and was replaced by r3 (big endian values), else r0 = 0

	// ---- floating point (Float registers) ----
	FMOV,       // d = a (both lanes)
	FMOVLO,     // d.ps0 = a.ps0, d.ps1 kept
	FSIGN,      // d.ps0 = sign op(a.ps0), d.ps1 kept. sub = IMLSign
	PSIGN,      // both lanes. sub = IMLSign
	PMERGE,     // d.ps0 = a.(sub >> 1), d.ps1 = b.(sub & 1)
	FARITH,     // d.ps0 = a op b (double), d.ps1 kept. sub = IMLArith, MUL uses b as frC
	FARITHS,    // d.ps0 = d.ps1 = single(a op b)
	FMADD,      // d.ps0 = a*c +- b (r1=a, r2=c, r3=b). sub = IMLMadd. d.ps1 kept
	FMADDS,     // single precision form, result in both lanes
	PARITH,     // paired a op b
	PMADD,      // paired multiply-add. sub = IMLMadd
	PMULS,      // d = a * c.(lane sub)
	PMADDS,     // d = a * c.(lane sub) + b
	PSUM,       // ps_sum0 (sub 0) / ps_sum1 (sub 1): r1=a, r2=b, r3=c
	FSEL,       // d = a.ps0 >= -0.0 ? c : b (whole register). r1=a, r2=c, r3=b
	PSEL,       // per lane
	FRSP,       // d.ps0 = d.ps1 = single(a.ps0)
	FCTIWZ,     // d.ps0 = fctiwz(a.ps0), d.ps1 kept
	FCTIW,      // d.ps0 = fctiw(a.ps0) with FPSCR[RN], d.ps1 kept
	FRES,       // d.ps0 = d.ps1 = fres(a.ps0)
	FRSQRTE,    // d.ps0 = frsqrte(a.ps0), d.ps1 kept
	PRES,       // paired fres, rounded to single
	PRSQRTE,    // paired frsqrte, rounded to single
	FCMP,       // r0-r3 = LT/GT/EQ/UN of a.(lane) vs b.(lane), r4 = a, r5 = b. Updates FPSCR[FPCC]
	MFTB,       // r0 = time base, lower (sub 0) or upper (sub 1) half

	// ---- home slot access (spill code and register writeback) ----
	HOMELOAD,   // r0 = home slot of virtual register imm
	HOMESTORE,  // home slot of virtual register imm = r0

	// ---- control flow ----
	CYCLES,     // remainingCycles -= imm
	JUMP,       // go to the taken successor
	BRANCH,     // go to the taken successor if (r0 != 0) == (cond == NE)
	BRANCHCMP,  // go to the taken successor if cond(a, b)
	CYCLECHECK, // go to the taken successor (an exit) if remainingCycles < 0
	EXITB,      // leave the function and continue at guest address addr
	EXITBR,     // leave and continue at the guest address in r0
	EXITLEAVE,  // return to the interpreter at guest address addr
	HLE,        // run HLE function (imm = opcode) at addr, then continue at the new IP
	INTERP,     // run the guest instruction at addr in the interpreter

	COUNT
};

// sub operations
enum class IMLFMem : uint8
{
	SINGLE,     // stfs: ps0 narrowed to single
	DOUBLE,     // stfd
	IWX,        // stfiwx: low word of ps0
};

enum class IMLSign : uint8 { NEG, ABS, NABS };
enum class IMLArith : uint8 { ADD, SUB, MUL, DIV };
enum class IMLMadd : uint8 { MADD, MSUB, NMADD, NMSUB };

// flags
enum : uint8
{
	IMLF_IMM = 0x01,     // second source is imm instead of r[2]
	IMLF_SIGNEXT = 0x02, // LOAD: sign extend
	IMLF_BYTEREV = 0x04, // LOAD/STORE: little endian access (lwbrx etc.)
	IMLF_INV_A = 0x08,   // ADDC/ADDE: complement a
	IMLF_PSQ_W = 0x10,   // PSQLOAD/PSQSTORE: single slot
	IMLF_ORDERED = 0x20, // FCMP: ordered compare (no VXSNAN update)
};

// role of an operand slot
enum : uint8
{
	IMLRole_None = 0,
	IMLRole_Use = 1,
	IMLRole_Def = 2,
	IMLRole_UseDef = 3,
};

constexpr int IML_OPERANDS = 6;

struct IMLOpInfo
{
	const char* name;
	uint8 roles[IML_OPERANDS];
	bool sideEffects;  // must not be removed even when its results are unused
	bool callsHelper;  // clobbers the host registers a helper call may clobber
	bool terminator;   // ends a block
	bool leaves;       // leaves the function (guest state must be in memory)
	bool barrier;      // guest state must be in memory before, registers are stale after
};

const IMLOpInfo& IMLOp_GetInfo(IMLOp op);

struct IMLInstruction
{
	IMLOp op{IMLOp::NOP};
	uint8 sub{0};      // sub operation, lane or condition (IMLCond)
	uint8 size{0};     // memory access size in bytes
	uint8 flags{0};
	IMLReg r[IML_OPERANDS]{IML_NOREG, IML_NOREG, IML_NOREG, IML_NOREG, IML_NOREG, IML_NOREG};
	sint32 imm{0};
	uint32 addr{0};    // guest address for exits, HLE and interpreter calls
	uint32 saveMask{0}; // helper calls: host registers to preserve around the call (set by the register allocator)

	const IMLOpInfo& Info() const { return IMLOp_GetInfo(op); }

	IMLCond Cond() const { return (IMLCond)sub; }
	bool HasImm() const { return (flags & IMLF_IMM) != 0; }

	bool IsUse(int slot) const { return r[slot] != IML_NOREG && (Info().roles[slot] & IMLRole_Use); }
	bool IsDef(int slot) const { return r[slot] != IML_NOREG && (Info().roles[slot] & IMLRole_Def); }

	// calls f(reg) for every register read / written by the instruction
	template<typename F>
	void ForEachUse(F f) const
	{
		const IMLOpInfo& info = Info();
		for (int i = 0; i < IML_OPERANDS; i++)
			if (r[i] != IML_NOREG && (info.roles[i] & IMLRole_Use))
				f(r[i]);
	}

	template<typename F>
	void ForEachDef(F f) const
	{
		const IMLOpInfo& info = Info();
		for (int i = 0; i < IML_OPERANDS; i++)
			if (r[i] != IML_NOREG && (info.roles[i] & IMLRole_Def))
				f(r[i]);
	}

	bool Reads(IMLReg reg) const
	{
		bool found = false;
		ForEachUse([&](IMLReg x) { found |= (x == reg); });
		return found;
	}

	bool Writes(IMLReg reg) const
	{
		bool found = false;
		ForEachDef([&](IMLReg x) { found |= (x == reg); });
		return found;
	}

	// ---- constructors used by the front end ----
	static IMLInstruction Make(IMLOp op, IMLReg r0 = IML_NOREG, IMLReg r1 = IML_NOREG, IMLReg r2 = IML_NOREG, IMLReg r3 = IML_NOREG)
	{
		IMLInstruction i;
		i.op = op;
		i.r[0] = r0;
		i.r[1] = r1;
		i.r[2] = r2;
		i.r[3] = r3;
		return i;
	}

	static IMLInstruction MakeImm(IMLOp op, IMLReg d, IMLReg a, sint32 imm)
	{
		IMLInstruction i;
		i.op = op;
		i.r[0] = d;
		i.r[1] = a;
		i.imm = imm;
		i.flags = IMLF_IMM;
		return i;
	}
};

// true for ops that may run with the second source as immediate
bool IMLOp_SupportsImm(IMLOp op);
