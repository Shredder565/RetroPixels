// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Register move instructions whose behaviour depends on the memory model
// (SPR set, time base source). Included by PPCInterpreterImpl.cpp only.

namespace EspressoItp
{
	// the SPR number is encoded with its two 5-bit halves swapped
	inline uint32 DecodeSprField(uint32 op)
	{
		return ((op >> 16) & 0x1F) | (((op >> 11) & 0x1F) << 5);
	}

	template<typename Bus>
	void Op_MFSPR(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 spr = DecodeSprField(op);
		uint32 value = Bus::kSupervisor ? ReadSprLLE(cpu, spr) : ReadSprHLE(cpu, spr);
		cpu->gpr[ppcOpField_D(op)] = value;
		PPCInterpreter_nextInstruction(cpu);
	}

	template<typename Bus>
	void Op_MTSPR(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 spr = DecodeSprField(op);
		uint32 value = cpu->gpr[ppcOpField_D(op)];
		if constexpr (Bus::kSupervisor)
			WriteSprLLE(cpu, spr, value);
		else
			WriteSprHLE(cpu, spr, value);
		PPCInterpreter_nextInstruction(cpu);
	}

	template<typename Bus>
	void Op_MFTB(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 spr = DecodeSprField(op);
		uint64 timeBase = Bus::TimeBase(cpu);
		if (spr == SPR_TBL)
			cpu->gpr[ppcOpField_D(op)] = (uint32)timeBase;
		else if (spr == SPR_TBU)
			cpu->gpr[ppcOpField_D(op)] = (uint32)(timeBase >> 32);
		else
			assert_dbg();
		PPCInterpreter_nextInstruction(cpu);
	}

	// segment registers (mfsr/mtsr): SR number in the low 4 bits of the rA field
	inline void Op_MFSR(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->gpr[ppcOpField_D(op)] = cpu->sprExtended.sr[ppcOpField_A(op) & 0xF];
		PPCInterpreter_nextInstruction(cpu);
	}

	inline void Op_MTSR(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->sprExtended.sr[ppcOpField_A(op) & 0xF] = cpu->gpr[ppcOpField_D(op)];
		PPCInterpreter_nextInstruction(cpu);
	}
}
