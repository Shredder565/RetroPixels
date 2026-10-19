// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "IML.h"

void IMLAnalyzer_ComputeLiveness(IMLFunction& func)
{
	const size_t regCount = func.RegCount();
	const size_t blockCount = func.blocks.size();
	func.Renumber();
	// per block: registers read before written (gen) and registers written (kill)
	std::vector<IMLRegSet> gen(blockCount), kill(blockCount);
	for (IMLSegment* b : func.blocks)
	{
		IMLRegSet& g = gen[b->index];
		IMLRegSet& k = kill[b->index];
		g.Resize(regCount);
		k.Resize(regCount);
		for (const IMLInstruction& inst : b->insts)
		{
			inst.ForEachUse([&](IMLReg r)
			{
				if (!k.Has(r))
					g.Add(r);
			});
			inst.ForEachDef([&](IMLReg r) { k.Add(r); });
		}
		b->liveIn.Resize(regCount);
		b->liveOut.Resize(regCount);
	}
	// iterate to a fixed point, visiting blocks from last to first
	bool changed = true;
	IMLRegSet scratch;
	scratch.Resize(regCount);
	while (changed)
	{
		changed = false;
		for (size_t i = blockCount; i-- > 0;)
		{
			IMLSegment* b = func.blocks[i];
			b->ForEachSuccessor([&](IMLSegment* s) { b->liveOut.Merge(s->liveIn); });
			scratch = b->liveOut;
			scratch.Subtract(kill[i]);
			scratch.Merge(gen[i]);
			if (!(scratch == b->liveIn))
			{
				b->liveIn = scratch;
				changed = true;
			}
		}
	}
}

void IMLAnalyzer_ComputeLoopDepth(IMLFunction& func)
{
	for (IMLSegment* b : func.blocks)
		b->loopDepth = 0;
	for (IMLSegment* b : func.blocks)
	{
		if (b->ppcAddress == 0)
			continue;
		b->ForEachSuccessor([&](IMLSegment* s)
		{
			if (s->ppcAddress == 0 || s->ppcAddress > b->ppcAddress)
				return;
			// guest blocks between the loop header and the branch are in the loop
			for (IMLSegment* x : func.blocks)
			{
				if (x->ppcAddress != 0 && x->ppcAddress >= s->ppcAddress && x->ppcAddress <= b->ppcAddress)
					x->loopDepth++;
			}
		});
	}
	// blocks made by the compiler inherit the depth of their predecessors
	for (IMLSegment* b : func.blocks)
	{
		if (b->ppcAddress != 0)
			continue;
		for (IMLSegment* p : b->preds)
			b->loopDepth = std::max(b->loopDepth, p->loopDepth);
	}
}

bool IMLAnalyzer_Verify(IMLFunction& func, std::string* problem)
{
	auto fail = [&](const std::string& text)
	{
		if (problem)
			*problem = text;
		return false;
	};
	for (IMLSegment* b : func.blocks)
	{
		for (size_t i = 0; i < b->insts.size(); i++)
		{
			const IMLInstruction& inst = b->insts[i];
			if (inst.Info().terminator && i + 1 != b->insts.size())
				return fail(fmt::format("block {}: terminator not at the end", b->index));
			for (int s = 0; s < IML_OPERANDS; s++)
			{
				if (inst.r[s] == IML_NOREG)
					continue;
				if (inst.r[s] >= func.RegCount())
					return fail(fmt::format("block {}: bad register", b->index));
				if (inst.Info().roles[s] == IMLRole_None)
					return fail(fmt::format("block {}: register in unused slot", b->index));
			}
		}
		IMLInstruction* t = b->Terminator();
		if (t)
		{
			bool conditional = t->op == IMLOp::BRANCH || t->op == IMLOp::BRANCHCMP || t->op == IMLOp::CYCLECHECK;
			if (t->Info().leaves && (b->taken || b->next))
				return fail(fmt::format("block {}: exit with successors", b->index));
			if (t->op == IMLOp::JUMP && (!b->taken || b->next))
				return fail(fmt::format("block {}: bad jump links", b->index));
			if (conditional && (!b->taken || !b->next))
				return fail(fmt::format("block {}: conditional branch without both successors", b->index));
		}
		else if (!b->next || b->taken)
			return fail(fmt::format("block {}: a block without terminator needs exactly a next block", b->index));
		if (b->isEntry && !b->preds.empty())
			return fail(fmt::format("block {}: entry block with predecessors", b->index));
	}
	return true;
}
