// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "IMLRegisterAllocatorRanges.h"

void IMLInterferenceGraph::AddEdge(IMLReg a, IMLReg b)
{
	if (a == b)
		return;
	m_adjacency[a].push_back(b);
	m_adjacency[b].push_back(a);
}

void IMLInterferenceGraph::Build(IMLFunction& func)
{
	const size_t regCount = func.RegCount();
	m_adjacency.assign(regCount, {});
	m_cost.assign(regCount, 0.0f);
	m_hint.assign(regCount, IML_NOREG);
	m_crossesCall.assign(regCount, 0);
	m_occurs.assign(regCount, 0);

	for (IMLSegment* b : func.blocks)
	{
		float weight = 1.0f;
		for (uint32 d = 0; d < std::min<uint32>(b->loopDepth, 4); d++)
			weight *= 8.0f;
		IMLRegSet live = b->liveOut;
		for (size_t i = b->insts.size(); i-- > 0;)
		{
			const IMLInstruction& inst = b->insts[i];
			IMLReg copySource = IML_NOREG;
			if (inst.op == IMLOp::MOV || inst.op == IMLOp::FMOV)
			{
				copySource = inst.r[1];
				m_hint[inst.r[0]] = inst.r[1];
				m_hint[inst.r[1]] = inst.r[0];
			}
			// registers live after a helper call (and not written by it) must survive the call
			if (inst.Info().callsHelper)
			{
				live.ForEach([&](IMLReg r)
				{
					if (!inst.Writes(r))
						m_crossesCall[r] = 1;
				});
			}
			// every register written here interferes with everything live after
			// the instruction, and with the other registers it writes
			inst.ForEachDef([&](IMLReg d)
			{
				m_occurs[d] = 1;
				m_cost[d] += weight;
				IMLRegKind kind = func.KindOf(d);
				live.ForEach([&](IMLReg x)
				{
					if (x != copySource && func.KindOf(x) == kind)
						AddEdge(d, x);
				});
				inst.ForEachDef([&](IMLReg other)
				{
					if (other != d && func.KindOf(other) == kind)
						AddEdge(d, other);
				});
			});
			inst.ForEachDef([&](IMLReg d) { live.Remove(d); });
			inst.ForEachUse([&](IMLReg u)
			{
				m_occurs[u] = 1;
				m_cost[u] += weight;
				live.Add(u);
			});
		}
		// registers live at the start of an entry block are loaded there; they
		// still must not share a host register with each other
		if (b->isEntry)
		{
			std::vector<IMLReg> liveIn;
			live.ForEach([&](IMLReg r) { liveIn.push_back(r); });
			for (size_t x = 0; x < liveIn.size(); x++)
			{
				for (size_t y = x + 1; y < liveIn.size(); y++)
				{
					if (func.KindOf(liveIn[x]) == func.KindOf(liveIn[y]))
						AddEdge(liveIn[x], liveIn[y]);
				}
			}
		}
	}
	for (auto& list : m_adjacency)
	{
		std::sort(list.begin(), list.end());
		list.erase(std::unique(list.begin(), list.end()), list.end());
	}
}
