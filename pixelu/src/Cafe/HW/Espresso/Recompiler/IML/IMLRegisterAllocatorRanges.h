// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "IML.h"

// Interference between virtual registers. Two registers interfere when one
// of them is written while the other one is live; such registers cannot
// share a host register. Built from the liveness information of the
// function (IMLAnalyzer_ComputeLiveness must have run).
class IMLInterferenceGraph
{
public:
	void Build(IMLFunction& func);

	const std::vector<IMLReg>& Neighbors(IMLReg r) const { return m_adjacency[r]; }

	// how valuable it is to keep the register in a host register
	float SpillCost(IMLReg r) const { return m_cost[r]; }
	// a register this one is copied from or to (preferred host register)
	IMLReg MoveHint(IMLReg r) const { return m_hint[r]; }
	// true if the register is live across an instruction that calls a helper
	bool CrossesCall(IMLReg r) const { return m_crossesCall[r] != 0; }
	// true if the register occurs in the code at all
	bool IsUsed(IMLReg r) const { return m_occurs[r] != 0; }

private:
	void AddEdge(IMLReg a, IMLReg b);

	std::vector<std::vector<IMLReg>> m_adjacency;
	std::vector<float> m_cost;
	std::vector<IMLReg> m_hint;
	std::vector<uint8> m_crossesCall;
	std::vector<uint8> m_occurs;
};
