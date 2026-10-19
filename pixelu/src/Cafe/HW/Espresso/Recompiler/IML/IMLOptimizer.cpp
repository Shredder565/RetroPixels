// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "IML.h"

// Guest registers are only observable through their home slots, so this
// pass must run after the home slot stores were added (see
// IMLRegisterAllocator_PrepareGuestRegisters): every guest value that can be
// seen outside of the function is then read by a HOMESTORE.
void IMLOptimizer_RemoveDeadCode(IMLFunction& func)
{
	bool changed = true;
	while (changed)
	{
		changed = false;
		IMLAnalyzer_ComputeLiveness(func);
		for (IMLSegment* b : func.blocks)
		{
			IMLRegSet live = b->liveOut;
			std::vector<IMLInstruction>& insts = b->insts;
			std::vector<bool> keep(insts.size(), true);
			for (size_t i = insts.size(); i-- > 0;)
			{
				const IMLInstruction& inst = insts[i];
				const IMLOpInfo& info = inst.Info();
				if (inst.op == IMLOp::NOP)
				{
					keep[i] = false;
					changed = true;
					continue;
				}
				if (!info.sideEffects && !info.terminator && !info.callsHelper)
				{
					bool anyLive = false;
					bool anyDef = false;
					inst.ForEachDef([&](IMLReg r)
					{
						anyDef = true;
						anyLive |= live.Has(r);
					});
					if (anyDef && !anyLive)
					{
						keep[i] = false;
						changed = true;
						continue;
					}
				}
				inst.ForEachDef([&](IMLReg r) { live.Remove(r); });
				inst.ForEachUse([&](IMLReg r) { live.Add(r); });
			}
			size_t out = 0;
			for (size_t i = 0; i < insts.size(); i++)
			{
				if (keep[i])
					insts[out++] = insts[i];
			}
			insts.resize(out);
		}
	}
}

void IMLOptimizer_Run(IMLFunction& func)
{
	IMLOptimizer_RemoveDeadCode(func);
}
