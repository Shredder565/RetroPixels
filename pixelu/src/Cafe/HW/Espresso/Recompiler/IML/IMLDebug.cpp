// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "IML.h"

std::string IMLDebug_FormatReg(const IMLFunction& func, IMLReg r)
{
	using namespace IMLGuest;
	if (r == IML_NOREG)
		return "-";
	std::string name;
	if (r < FPR0)
		name = fmt::format("r{}", r - GPR0);
	else if (r < CR0)
		name = fmt::format("f{}", r - FPR0);
	else if (r < XER_CA)
		name = fmt::format("cr{}.{}", (r - CR0) / 4, "lges"[(r - CR0) % 4]);
	else if (r == XER_CA)
		name = "ca";
	else if (r == XER_OV)
		name = "ov";
	else if (r == XER_SO)
		name = "so";
	else if (r == LR)
		name = "lr";
	else if (r == CTR)
		name = "ctr";
	else if (r >= GQR0 && r < GQR0 + 8)
		name = fmt::format("gqr{}", r - GQR0);
	else if (r == RESV_ADDR)
		name = "resvaddr";
	else if (r == RESV_VALUE)
		name = "resvval";
	else
		name = fmt::format("{}{}", func.KindOf(r) == IMLRegKind::Float ? "ft" : "t", r);
	if (r < func.RegCount())
	{
		const IMLVRegInfo& info = func.vregs[r];
		if (info.spilled)
			name += "[mem]";
		else if (info.hostReg >= 0)
			name += fmt::format("[{}]", info.hostReg);
	}
	return name;
}

std::string IMLDebug_FormatInstruction(const IMLFunction& func, const IMLInstruction& inst)
{
	static const char* kCondNames[] = {"eq", "ne", "slt", "sgt", "sle", "sge", "ult", "ugt", "ule", "uge"};
	const IMLOpInfo& info = inst.Info();
	std::string s = info.name;
	if (inst.op == IMLOp::BRANCH || inst.op == IMLOp::BRANCHCMP || inst.op == IMLOp::CMPSET || inst.op == IMLOp::CMPCR)
		s += fmt::format(".{}", inst.sub < 10 ? kCondNames[inst.sub] : "?");
	else if (inst.sub)
		s += fmt::format(".{}", inst.sub);
	if (inst.size)
		s += fmt::format(" size={}", inst.size);
	bool first = true;
	for (int i = 0; i < IML_OPERANDS; i++)
	{
		if (inst.r[i] == IML_NOREG)
			continue;
		s += first ? " " : ", ";
		first = false;
		if (info.roles[i] & IMLRole_Def)
			s += (info.roles[i] & IMLRole_Use) ? "*" : ">";
		s += IMLDebug_FormatReg(func, inst.r[i]);
	}
	if (inst.HasImm() || inst.imm != 0 || inst.op == IMLOp::MOVI)
		s += fmt::format(" #{:x}", (uint32)inst.imm);
	if (inst.op == IMLOp::HOMELOAD || inst.op == IMLOp::HOMESTORE)
		s += fmt::format(" home({})", IMLDebug_FormatReg(func, (IMLReg)inst.imm));
	if (inst.flags)
		s += fmt::format(" flags={:x}", inst.flags);
	if (inst.addr)
		s += fmt::format(" @{:08x}", inst.addr);
	if (inst.saveMask)
		s += fmt::format(" save={:x}", inst.saveMask);
	return s;
}

std::string IMLDebug_FormatFunction(const IMLFunction& func)
{
	std::string s = fmt::format("function {:08x}\n", func.entryAddress);
	for (const IMLSegment* b : func.blocks)
	{
		s += fmt::format("block {} ppc {:08x}-{:08x}{} depth {}", b->index, b->ppcAddress, b->ppcEndAddress, b->isEntry ? fmt::format(" ENTRY {:08x}", b->entryAddress) : "", b->loopDepth);
		if (b->taken)
			s += fmt::format(" taken {}", b->taken->index);
		if (b->next)
			s += fmt::format(" next {}", b->next->index);
		s += "\n";
		for (const IMLInstruction& inst : b->insts)
			s += "    " + IMLDebug_FormatInstruction(func, inst) + "\n";
	}
	return s;
}

void IMLDebug_Dump(const IMLFunction& func)
{
	std::string s = IMLDebug_FormatFunction(func);
	fputs(s.c_str(), stdout);
	fflush(stdout);
}
