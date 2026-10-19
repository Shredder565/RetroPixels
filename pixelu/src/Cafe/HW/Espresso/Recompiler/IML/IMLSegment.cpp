// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Espresso/PPCState.h"
#include "IMLSegment.h"

sint32 IMLGuest_HomeOffset(IMLReg r)
{
	using namespace IMLGuest;
	if (r < FPR0)
		return (sint32)(offsetof(PPCInterpreter_t, gpr) + (r - GPR0) * sizeof(uint32));
	if (r < CR0)
		return (sint32)(offsetof(PPCInterpreter_t, fpr) + (r - FPR0) * sizeof(FPR_t));
	if (r < XER_CA)
		return (sint32)(offsetof(PPCInterpreter_t, cr) + (r - CR0));
	switch (r)
	{
	case XER_CA: return (sint32)offsetof(PPCInterpreter_t, xer_ca);
	case XER_OV: return (sint32)offsetof(PPCInterpreter_t, xer_ov);
	case XER_SO: return (sint32)offsetof(PPCInterpreter_t, xer_so);
	case LR: return (sint32)offsetof(PPCInterpreter_t, spr.LR);
	case CTR: return (sint32)offsetof(PPCInterpreter_t, spr.CTR);
	case RESV_ADDR: return (sint32)offsetof(PPCInterpreter_t, reservedMemAddr);
	case RESV_VALUE: return (sint32)offsetof(PPCInterpreter_t, reservedMemValue);
	default:
		break;
	}
	cemu_assert_debug(r >= GQR0 && r < GQR0 + 8);
	return (sint32)(offsetof(PPCInterpreter_t, spr.UGQR) + (r - GQR0) * sizeof(uint32));
}

uint8 IMLGuest_HomeSize(IMLReg r)
{
	using namespace IMLGuest;
	if (r >= FPR0 && r < CR0)
		return 16;
	if (r >= CR0 && r <= XER_SO)
		return 1;
	return 4;
}

IMLFunction::IMLFunction()
{
	vregs.resize(IMLGuest::COUNT);
	for (IMLReg r = 0; r < IMLGuest::COUNT; r++)
	{
		IMLVRegInfo& info = vregs[r];
		info.kind = (r >= IMLGuest::FPR0 && r < IMLGuest::CR0) ? IMLRegKind::Float : IMLRegKind::Int;
		info.home = IMLGuest_HomeOffset(r);
		info.homeSize = IMLGuest_HomeSize(r);
	}
}

IMLFunction::~IMLFunction()
{
	for (IMLSegment* b : blocks)
		delete b;
}

IMLSegment* IMLFunction::NewBlock()
{
	IMLSegment* b = new IMLSegment();
	b->index = (uint32)blocks.size();
	blocks.push_back(b);
	return b;
}

IMLSegment* IMLFunction::InsertBlockAfter(IMLSegment* after)
{
	IMLSegment* b = new IMLSegment();
	auto it = std::find(blocks.begin(), blocks.end(), after);
	cemu_assert_debug(it != blocks.end());
	blocks.insert(it + 1, b);
	Renumber();
	return b;
}

void IMLFunction::Renumber()
{
	for (size_t i = 0; i < blocks.size(); i++)
		blocks[i]->index = (uint32)i;
}

void IMLFunction::RebuildPredecessors()
{
	for (IMLSegment* b : blocks)
		b->preds.clear();
	for (IMLSegment* b : blocks)
		b->ForEachSuccessor([b](IMLSegment* s) { s->preds.push_back(b); });
}

// drops blocks that no entry point can reach
void IMLFunction::RemoveUnreachable()
{
	std::vector<uint8> reached(blocks.size(), 0);
	std::vector<IMLSegment*> stack;
	Renumber();
	for (IMLSegment* b : blocks)
	{
		if (b->isEntry)
		{
			reached[b->index] = 1;
			stack.push_back(b);
		}
	}
	while (!stack.empty())
	{
		IMLSegment* b = stack.back();
		stack.pop_back();
		b->ForEachSuccessor([&](IMLSegment* s)
		{
			if (!reached[s->index])
			{
				reached[s->index] = 1;
				stack.push_back(s);
			}
		});
	}
	std::vector<IMLSegment*> kept;
	for (IMLSegment* b : blocks)
	{
		if (reached[b->index])
			kept.push_back(b);
		else
			delete b;
	}
	blocks = std::move(kept);
	Renumber();
	RebuildPredecessors();
}

IMLReg IMLFunction::NewTemp(IMLRegKind kind)
{
	cemu_assert(vregs.size() < 0xFFF0);
	IMLVRegInfo info;
	info.kind = kind;
	info.home = -1;
	info.homeSize = kind == IMLRegKind::Float ? 16 : 4;
	vregs.push_back(info);
	return (IMLReg)(vregs.size() - 1);
}
