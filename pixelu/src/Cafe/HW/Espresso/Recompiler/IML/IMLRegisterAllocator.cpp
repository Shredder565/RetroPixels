// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "IMLRegisterAllocator.h"
#include "IMLRegisterAllocatorRanges.h"

namespace
{
	IMLInstruction MakeHomeLoad(IMLReg dst, IMLReg owner)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::HOMELOAD, dst);
		i.imm = (sint32)owner;
		return i;
	}

	IMLInstruction MakeHomeStore(IMLReg src, IMLReg owner)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::HOMESTORE, src);
		i.imm = (sint32)owner;
		return i;
	}

	// guest registers live right after each instruction of a block
	std::vector<IMLRegSet> LiveAfterEachInstruction(IMLSegment* b)
	{
		std::vector<IMLRegSet> result(b->insts.size());
		IMLRegSet live = b->liveOut;
		for (size_t i = b->insts.size(); i-- > 0;)
		{
			result[i] = live;
			const IMLInstruction& inst = b->insts[i];
			inst.ForEachDef([&](IMLReg r) { live.Remove(r); });
			inst.ForEachUse([&](IMLReg r) { live.Add(r); });
		}
		return result;
	}
}

void IMLRegisterAllocator_PrepareGuestRegisters(IMLFunction& func)
{
	IMLAnalyzer_ComputeLiveness(func);

	// loads: guest registers the code reads before writing them are loaded at
	// the entry points; after an interpreter call every guest register the
	// code still reads is loaded again
	for (IMLSegment* b : func.blocks)
	{
		std::vector<IMLRegSet> liveAfter = LiveAfterEachInstruction(b);
		std::vector<IMLInstruction> out;
		out.reserve(b->insts.size() + 8);
		if (b->isEntry)
		{
			b->liveIn.ForEach([&](IMLReg r)
			{
				cemu_assert_debug(IMLGuest::IsGuest(r)); // temporaries are always written first
				if (IMLGuest::IsGuest(r))
					out.push_back(MakeHomeLoad(r, r));
			});
		}
		for (size_t i = 0; i < b->insts.size(); i++)
		{
			out.push_back(b->insts[i]);
			if (b->insts[i].op == IMLOp::INTERP)
			{
				liveAfter[i].ForEach([&](IMLReg r)
				{
					if (IMLGuest::IsGuest(r))
						out.push_back(MakeHomeLoad(r, r));
				});
			}
		}
		b->insts = std::move(out);
	}

	// stores: every write of a guest register is followed by a store to its
	// home slot, unless the same block writes the register again before the
	// next point where the home slot can be observed
	for (IMLSegment* b : func.blocks)
	{
		std::vector<IMLInstruction> out;
		out.reserve(b->insts.size() * 2);
		for (const IMLInstruction& inst : b->insts)
		{
			out.push_back(inst);
			if (inst.op == IMLOp::HOMELOAD)
				continue;
			inst.ForEachDef([&](IMLReg r)
			{
				if (IMLGuest::IsGuest(r))
					out.push_back(MakeHomeStore(r, r));
			});
		}
		// backwards: drop stores that a later store of the same register replaces
		std::vector<uint8> storedLater(IMLGuest::COUNT, 0);
		std::vector<bool> keep(out.size(), true);
		for (size_t i = out.size(); i-- > 0;)
		{
			const IMLInstruction& inst = out[i];
			if (inst.Info().barrier || inst.Info().leaves)
			{
				std::fill(storedLater.begin(), storedLater.end(), 0);
				continue;
			}
			if (inst.op == IMLOp::HOMESTORE && IMLGuest::IsGuest((IMLReg)inst.imm))
			{
				if (storedLater[inst.imm])
					keep[i] = false;
				storedLater[inst.imm] = 1;
			}
			else if (inst.op == IMLOp::HOMELOAD && IMLGuest::IsGuest((IMLReg)inst.imm))
				storedLater[inst.imm] = 0;
		}
		b->insts.clear();
		for (size_t i = 0; i < out.size(); i++)
		{
			if (keep[i])
				b->insts.push_back(out[i]);
		}
	}
}

namespace
{
	struct Allocator
	{
		IMLFunction& func;
		const IMLRegisterAllocatorParameters& params;
		IMLInterferenceGraph graph;
		std::vector<sint16> color;

		Allocator(IMLFunction& f, const IMLRegisterAllocatorParameters& p) : func(f), params(p) {}

		uint32 ColorCount(IMLReg r) const
		{
			return func.KindOf(r) == IMLRegKind::Float ? params.floatRegCount : params.intRegCount;
		}

		uint32 CallerSavedMask(IMLReg r) const
		{
			return func.KindOf(r) == IMLRegKind::Float ? params.floatCallerSavedMask : params.intCallerSavedMask;
		}

		// returns the registers that did not get a color
		std::vector<IMLReg> Color()
		{
			const size_t regCount = func.RegCount();
			color.assign(regCount, -1);
			std::vector<IMLReg> order;
			for (IMLReg r = 0; r < regCount; r++)
			{
				if (graph.IsUsed(r) && !func.vregs[r].spilled)
					order.push_back(r);
			}
			std::stable_sort(order.begin(), order.end(), [&](IMLReg a, IMLReg b)
			{
				bool na = func.vregs[a].noSpill, nb = func.vregs[b].noSpill;
				if (na != nb)
					return na;
				return graph.SpillCost(a) > graph.SpillCost(b);
			});
			std::vector<IMLReg> uncolored;
			for (IMLReg r : order)
			{
				uint32 count = ColorCount(r);
				uint32 used = 0;
				for (IMLReg n : graph.Neighbors(r))
				{
					if (color[n] >= 0)
						used |= 1u << color[n];
				}
				uint32 freeMask = ((count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1)) & ~used;
				if (freeMask == 0)
				{
					uncolored.push_back(r);
					continue;
				}
				sint32 pick = -1;
				IMLReg hint = graph.MoveHint(r);
				if (hint != IML_NOREG && color[hint] >= 0 && (freeMask & (1u << color[hint])))
					pick = color[hint];
				if (pick < 0)
				{
					// registers that live across helper calls prefer registers the call preserves
					uint32 callerSaved = CallerSavedMask(r);
					uint32 preferred = graph.CrossesCall(r) ? (freeMask & ~callerSaved) : (freeMask & callerSaved);
					uint32 m = preferred ? preferred : freeMask;
					pick = std::countr_zero(m);
				}
				color[r] = (sint16)pick;
			}
			return uncolored;
		}

		// moves the registers into memory: every access goes through a new short lived temporary
		void Spill(const std::vector<IMLReg>& regs)
		{
			std::vector<uint8> isSpilled(func.RegCount(), 0);
			for (IMLReg r : regs)
			{
				IMLVRegInfo& info = func.vregs[r];
				info.spilled = true;
				if (!IMLGuest::IsGuest(r))
					info.home = (sint32)func.AllocSpillSlot();
				isSpilled[r] = 1;
			}
			for (IMLSegment* b : func.blocks)
			{
				std::vector<IMLInstruction> out;
				out.reserve(b->insts.size() + 16);
				for (IMLInstruction inst : b->insts)
				{
					if ((inst.op == IMLOp::HOMELOAD || inst.op == IMLOp::HOMESTORE) && inst.r[0] < isSpilled.size() && isSpilled[inst.r[0]] && inst.imm == (sint32)inst.r[0])
						continue; // the register lives in its home slot now
					IMLReg replaced[IML_OPERANDS];
					IMLReg temps[IML_OPERANDS];
					int count = 0;
					bool uses[IML_OPERANDS]{};
					bool defs[IML_OPERANDS]{};
					for (int s = 0; s < IML_OPERANDS; s++)
					{
						IMLReg r = inst.r[s];
						if (r == IML_NOREG || r >= isSpilled.size() || !isSpilled[r])
							continue;
						int k = 0;
						while (k < count && replaced[k] != r)
							k++;
						if (k == count)
						{
							replaced[count] = r;
							IMLReg t = func.NewTemp(func.KindOf(r));
							func.vregs[t].noSpill = true;
							temps[count] = t;
							count++;
						}
						if (inst.IsUse(s))
							uses[k] = true;
						if (inst.IsDef(s))
							defs[k] = true;
						inst.r[s] = temps[k];
					}
					for (int k = 0; k < count; k++)
					{
						if (uses[k])
							out.push_back(MakeHomeLoad(temps[k], replaced[k]));
					}
					out.push_back(inst);
					for (int k = 0; k < count; k++)
					{
						if (defs[k])
							out.push_back(MakeHomeStore(temps[k], replaced[k]));
					}
				}
				b->insts = std::move(out);
			}
		}

		void ComputeSaveMasks()
		{
			IMLAnalyzer_ComputeLiveness(func);
			for (IMLSegment* b : func.blocks)
			{
				IMLRegSet live = b->liveOut;
				for (size_t i = b->insts.size(); i-- > 0;)
				{
					IMLInstruction& inst = b->insts[i];
					if (inst.Info().callsHelper)
					{
						uint32 mask = 0;
						IMLRegSet keep = live;
						// a register that is read and written keeps part of its old
						// value (lane 1), so it is preserved like an input
						inst.ForEachUse([&](IMLReg r) { if (inst.Writes(r)) keep.Add(r); });
						keep.ForEach([&](IMLReg r)
						{
							if (inst.Writes(r) && !inst.Reads(r))
								return;
							sint16 c = func.vregs[r].hostReg;
							if (c < 0)
								return;
							if (func.KindOf(r) == IMLRegKind::Float)
							{
								if (params.floatCallerSavedMask & (1u << c))
									mask |= 1u << (c + IML_SAVEMASK_FLOAT_SHIFT);
							}
							else if (params.intCallerSavedMask & (1u << c))
								mask |= 1u << c;
						});
						inst.saveMask = mask;
					}
					inst.ForEachDef([&](IMLReg r) { live.Remove(r); });
					inst.ForEachUse([&](IMLReg r) { live.Add(r); });
				}
			}
		}

		bool Run()
		{
			for (int attempt = 0; attempt < 12; attempt++)
			{
				IMLAnalyzer_ComputeLiveness(func);
				graph.Build(func);
				std::vector<IMLReg> uncolored = Color();
				if (uncolored.empty())
				{
					for (IMLReg r = 0; r < func.RegCount(); r++)
						func.vregs[r].hostReg = color[r];
					ComputeSaveMasks();
					return true;
				}
				for (IMLReg r : uncolored)
				{
					if (func.vregs[r].noSpill)
						return false; // should not happen: too many operands live at once
				}
				Spill(uncolored);
			}
			return false;
		}
	};
}

bool IMLRegisterAllocator_AllocateRegisters(IMLFunction& func, const IMLRegisterAllocatorParameters& params)
{
	IMLAnalyzer_ComputeLoopDepth(func);
	Allocator allocator(func, params);
	return allocator.Run();
}
