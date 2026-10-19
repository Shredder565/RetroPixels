// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "IMLInstruction.h"

// Operand roles and properties of every intermediate op. The order of the
// rows must follow IMLOp.

namespace
{
	constexpr uint8 N = IMLRole_None;
	constexpr uint8 U = IMLRole_Use;
	constexpr uint8 D = IMLRole_Def;
	constexpr uint8 X = IMLRole_UseDef;

	struct Row
	{
		IMLOp op;
		IMLOpInfo info;
	};

	// name, roles r0..r5, side effects, helper call, terminator, leaves, barrier
	constexpr Row kRows[] =
	{
		{IMLOp::NOP,       {"nop",       {N, N, N, N, N, N}, false, false, false, false, false}},

		{IMLOp::MOV,       {"mov",       {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::MOVI,      {"movi",      {D, N, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::ADD,       {"add",       {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::SUB,       {"sub",       {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::AND,       {"and",       {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::OR,        {"or",        {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::XOR,       {"xor",       {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::ANDC,      {"andc",      {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::ORC,       {"orc",       {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::NOT,       {"not",       {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::NEG,       {"neg",       {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::MUL,       {"mul",       {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::MULHS,     {"mulhs",     {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::MULHU,     {"mulhu",     {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::DIVS,      {"divs",      {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::DIVU,      {"divu",      {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::SHL,       {"shl",       {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::SHR,       {"shr",       {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::SAR,       {"sar",       {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::ROTL,      {"rotl",      {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::SLW,       {"slw",       {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::SRW,       {"srw",       {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::SRAW,      {"sraw",      {D, D, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::EXTS8,     {"exts8",     {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::EXTS16,    {"exts16",    {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::CNTLZ,     {"cntlz",     {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::BSWAP32,   {"bswap32",   {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::BSWAP16,   {"bswap16",   {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::ADDC,      {"addc",      {D, D, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::ADDE,      {"adde",      {D, X, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::CMPSET,    {"cmpset",    {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::CMPCR,     {"cmpcr",     {D, D, D, U, U, N}, false, false, false, false, false}},

		{IMLOp::LOAD,      {"load",      {D, U, U, N, N, N}, true,  false, false, false, false}},
		{IMLOp::STORE,     {"store",     {U, U, U, N, N, N}, true,  false, false, false, false}},
		{IMLOp::LOADFS,    {"loadfs",    {D, U, U, N, N, N}, true,  false, false, false, false}},
		{IMLOp::LOADFD,    {"loadfd",    {X, U, U, N, N, N}, true,  false, false, false, false}},
		{IMLOp::STOREF,    {"storef",    {U, U, U, N, N, N}, true,  false, false, false, false}},
		{IMLOp::PSQLOAD,   {"psqload",   {D, U, U, U, N, N}, true,  true,  false, false, false}},
		{IMLOp::PSQSTORE,  {"psqstore",  {U, U, U, U, N, N}, true,  true,  false, false, false}},
		{IMLOp::ZEROLINE,  {"zeroline",  {N, U, U, N, N, N}, true,  false, false, false, false}},
		{IMLOp::CMPXCHG,   {"cmpxchg",   {D, U, U, U, U, N}, true,  false, false, false, false}},

		{IMLOp::FMOV,      {"fmov",      {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::FMOVLO,    {"fmovlo",    {X, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::FSIGN,     {"fsign",     {X, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::PSIGN,     {"psign",     {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::PMERGE,    {"pmerge",    {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::FARITH,    {"farith",    {X, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::FARITHS,   {"fariths",   {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::FMADD,     {"fmadd",     {X, U, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::FMADDS,    {"fmadds",    {D, U, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::PARITH,    {"parith",    {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::PMADD,     {"pmadd",     {D, U, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::PMULS,     {"pmuls",     {D, U, U, N, N, N}, false, false, false, false, false}},
		{IMLOp::PMADDS,    {"pmadds",    {D, U, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::PSUM,      {"psum",      {D, U, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::FSEL,      {"fsel",      {D, U, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::PSEL,      {"psel",      {D, U, U, U, N, N}, false, false, false, false, false}},
		{IMLOp::FRSP,      {"frsp",      {D, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::FCTIWZ,    {"fctiwz",    {X, U, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::FCTIW,     {"fctiw",     {X, U, N, N, N, N}, false, true,  false, false, false}},
		{IMLOp::FRES,      {"fres",      {D, U, N, N, N, N}, false, true,  false, false, false}},
		{IMLOp::FRSQRTE,   {"frsqrte",   {X, U, N, N, N, N}, false, true,  false, false, false}},
		{IMLOp::PRES,      {"pres",      {D, U, N, N, N, N}, false, true,  false, false, false}},
		{IMLOp::PRSQRTE,   {"prsqrte",   {D, U, N, N, N, N}, false, true,  false, false, false}},
		{IMLOp::FCMP,      {"fcmp",      {D, D, D, D, U, U}, true,  false, false, false, false}},
		{IMLOp::MFTB,      {"mftb",      {D, N, N, N, N, N}, true,  true,  false, false, false}},

		{IMLOp::HOMELOAD,  {"homeload",  {D, N, N, N, N, N}, false, false, false, false, false}},
		{IMLOp::HOMESTORE, {"homestore", {U, N, N, N, N, N}, true,  false, false, false, false}},

		{IMLOp::CYCLES,    {"cycles",    {N, N, N, N, N, N}, true,  false, false, false, false}},
		{IMLOp::JUMP,      {"jump",      {N, N, N, N, N, N}, true,  false, true,  false, false}},
		{IMLOp::BRANCH,    {"branch",    {U, N, N, N, N, N}, true,  false, true,  false, false}},
		{IMLOp::BRANCHCMP, {"branchcmp", {U, U, N, N, N, N}, true,  false, true,  false, false}},
		{IMLOp::CYCLECHECK,{"cyclecheck",{N, N, N, N, N, N}, true,  false, true,  false, false}},
		{IMLOp::EXITB,     {"exitb",     {N, N, N, N, N, N}, true,  false, true,  true,  false}},
		{IMLOp::EXITBR,    {"exitbr",    {U, N, N, N, N, N}, true,  false, true,  true,  false}},
		{IMLOp::EXITLEAVE, {"exitleave", {N, N, N, N, N, N}, true,  false, true,  true,  false}},
		{IMLOp::HLE,       {"hle",       {N, N, N, N, N, N}, true,  true,  true,  true,  true}},
		{IMLOp::INTERP,    {"interp",    {N, N, N, N, N, N}, true,  true,  false, false, true}},
	};

	static_assert(sizeof(kRows) / sizeof(kRows[0]) == (size_t)IMLOp::COUNT, "op table out of sync");

	constexpr bool RowsInOrder()
	{
		for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++)
		{
			if ((size_t)kRows[i].op != i)
				return false;
		}
		return true;
	}
	static_assert(RowsInOrder(), "op table out of order");
}

const IMLOpInfo& IMLOp_GetInfo(IMLOp op)
{
	return kRows[(size_t)op].info;
}

bool IMLOp_SupportsImm(IMLOp op)
{
	switch (op)
	{
	case IMLOp::ADD:
	case IMLOp::SUB:
	case IMLOp::AND:
	case IMLOp::OR:
	case IMLOp::XOR:
	case IMLOp::MUL:
	case IMLOp::ROTL:
	case IMLOp::SRAW:
	case IMLOp::ADDC:
	case IMLOp::ADDE:
	case IMLOp::CMPSET:
	case IMLOp::CMPCR:
	case IMLOp::BRANCHCMP:
		return true;
	default:
		return false;
	}
}

IMLCond IMLCond_Invert(IMLCond c)
{
	switch (c)
	{
	case IMLCond::EQ: return IMLCond::NE;
	case IMLCond::NE: return IMLCond::EQ;
	case IMLCond::SLT: return IMLCond::SGE;
	case IMLCond::SGT: return IMLCond::SLE;
	case IMLCond::SLE: return IMLCond::SGT;
	case IMLCond::SGE: return IMLCond::SLT;
	case IMLCond::ULT: return IMLCond::UGE;
	case IMLCond::UGT: return IMLCond::ULE;
	case IMLCond::ULE: return IMLCond::UGT;
	case IMLCond::UGE: return IMLCond::ULT;
	}
	return c;
}
