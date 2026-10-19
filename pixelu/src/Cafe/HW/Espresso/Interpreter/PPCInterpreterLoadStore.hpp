// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Load and store instructions, templated on the memory model (see
// PPCInterpreterImpl.cpp). Included by PPCInterpreterImpl.cpp only.
//
// Notes on details that existing guest code and saved behaviour rely on:
// - update forms with rA = 0 (invalid encodings) behave per instruction as
//   documented next to each handler
// - in the LLE model a data storage interrupt raised by an access sets
//   memoryException; only lwz, stwcx. and dcbz check it right away
// - some handlers advance the instruction pointer before accessing memory,
//   which is only observable when the access raises an interrupt

namespace EspressoItp
{
	inline uint32 EA_D(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		return (rA ? cpu->gpr[rA] : 0) + ppcOpSImm16(op);
	}

	inline uint32 EA_X(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		return (rA ? cpu->gpr[rA] : 0) + cpu->gpr[ppcOpField_B(op)];
	}

	// ---- integer stores ----

	// stw with rA = 0 does not store anything
	template<typename Bus>
	void Op_STW(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		if (rA != 0)
			Bus::Write32(cpu, cpu->gpr[rA] + ppcOpSImm16(op), cpu->gpr[ppcOpField_D(op)]);
		PPCInterpreter_nextInstruction(cpu);
	}

	// stwu and stbu always use and update rA (also r0)
	template<typename Bus, uint32 Size>
	void Op_StoreUpdateAlways(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		uint32 ea = cpu->gpr[rA] + ppcOpSImm16(op);
		uint32 value = cpu->gpr[ppcOpField_D(op)];
		if constexpr (Size == 4)
			Bus::Write32(cpu, ea, value);
		else
			Bus::Write8(cpu, ea, (uint8)value);
		cpu->gpr[rA] = ea;
		PPCInterpreter_nextInstruction(cpu);
	}

	// sth, stb, sthx, stbx, stwx, stwbrx, sthbrx and their update forms (rA = 0 is not updated)
	template<typename Bus, uint32 Size, bool Indexed, bool Update, bool ByteReverse>
	void Op_Store(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 ea = Indexed ? EA_X(cpu, op) : EA_D(cpu, op);
		uint32 value = cpu->gpr[ppcOpField_D(op)];
		if constexpr (Size == 4)
			Bus::Write32(cpu, ea, ByteReverse ? _swapEndianU32(value) : value);
		else if constexpr (Size == 2)
			Bus::Write16(cpu, ea, ByteReverse ? _swapEndianU16((uint16)value) : (uint16)value);
		else
			Bus::Write8(cpu, ea, (uint8)value);
		if constexpr (Update)
		{
			uint32 rA = ppcOpField_A(op);
			if (rA != 0)
				cpu->gpr[rA] = ea;
		}
		PPCInterpreter_nextInstruction(cpu);
	}

	template<typename Bus>
	void Op_STMW(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 ea = EA_D(cpu, op);
		for (uint32 r = ppcOpField_D(op); r < 32; r++, ea += 4)
			Bus::Write32(cpu, ea, cpu->gpr[r]);
		PPCInterpreter_nextInstruction(cpu);
	}

	// stswi/stswx: byteCount bytes from consecutive registers (wrapping to r0), MSB first
	template<typename Bus>
	void StoreString(PPCInterpreter_t* cpu, uint32 reg, uint32 ea, uint32 byteCount)
	{
		uint32 shiftReg = 0;
		for (uint32 i = 0; i < byteCount; i++)
		{
			if ((i & 3) == 0)
			{
				shiftReg = cpu->gpr[reg];
				reg = (reg + 1) & 31;
			}
			Bus::Write8(cpu, ea + i, (uint8)(shiftReg >> 24));
			shiftReg <<= 8;
		}
	}

	template<typename Bus>
	void Op_STSWI(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		uint32 count = ppcOpField_B(op);
		StoreString<Bus>(cpu, ppcOpField_D(op), rA ? cpu->gpr[rA] : 0, count == 0 ? 32 : count);
		PPCInterpreter_nextInstruction(cpu);
	}

	template<typename Bus>
	void Op_STSWX(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 count = cpu->spr.XER & 0x7F;
		if (count != 0)
			StoreString<Bus>(cpu, ppcOpField_D(op), EA_X(cpu, op), count);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- reservation ----

	template<typename Bus>
	void Op_LWARX(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 ea = EA_X(cpu, op);
		uint32 value = Bus::Read32(cpu, ea);
		cpu->gpr[ppcOpField_D(op)] = value;
		cpu->reservedMemAddr = ea;
		cpu->reservedMemValue = value;
		PPCInterpreter_nextInstruction(cpu);
	}

	// stwcx. succeeds when the reservation address matches and memory still
	// holds the value seen by lwarx (checked with an atomic compare-exchange
	// because other cores run on other host threads)
	template<typename Bus>
	void Op_STWCX(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 ea = EA_X(cpu, op);
		bool stored = false;
		if (cpu->reservedMemAddr == ea)
		{
			uint8* hostPtr = Bus::ReservationPointer(cpu, ea);
			if (Bus::ConsumeAccessFault(cpu))
				return;
			uint32be expected = cpu->reservedMemValue;
			uint32be desired = cpu->gpr[ppcOpField_D(op)];
			stored = _rawPtrToAtomic((uint32be*)hostPtr)->compare_exchange_strong(expected, desired);
			cpu->cr[CR_BIT_SO] = cpu->xer_so;
			cpu->reservedMemAddr = 0;
			cpu->reservedMemValue = 0;
		}
		// on a failed attempt without reservation CR0[SO] is left as is
		cpu->cr[CR_BIT_LT] = 0;
		cpu->cr[CR_BIT_GT] = 0;
		cpu->cr[CR_BIT_EQ] = stored ? 1 : 0;
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- integer loads ----

	// lwz is the one load that stops on a data storage interrupt
	template<typename Bus>
	void Op_LWZ(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 value = Bus::Read32(cpu, EA_D(cpu, op));
		if (Bus::ConsumeAccessFault(cpu))
			return;
		cpu->gpr[ppcOpField_D(op)] = value;
		PPCInterpreter_nextInstruction(cpu);
	}

	// lwzu: rA is updated first, then rD is loaded (rD wins if they are equal)
	template<typename Bus>
	void Op_LWZU(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		cpu->gpr[rA] += ppcOpSImm16(op);
		cpu->gpr[ppcOpField_D(op)] = Bus::Read32(cpu, cpu->gpr[rA]);
		PPCInterpreter_nextInstruction(cpu);
	}

	// lbzu: same order as lwzu
	template<typename Bus>
	void Op_LBZU(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		uint32 ea = cpu->gpr[rA] + ppcOpSImm16(op);
		cpu->gpr[rA] = ea;
		cpu->gpr[ppcOpField_D(op)] = Bus::Read8(cpu, ea);
		PPCInterpreter_nextInstruction(cpu);
	}

	// lhzu: rD is loaded first, then rA (also r0) is updated, so rA wins when rA = rD
	template<typename Bus>
	void Op_LHZU(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		uint32 imm = ppcOpSImm16(op);
		cpu->gpr[ppcOpField_D(op)] = Bus::Read16(cpu, cpu->gpr[rA] + imm);
		cpu->gpr[rA] += imm;
		PPCInterpreter_nextInstruction(cpu);
	}

	template<typename Bus, uint32 Size>
	uint32 LoadValue(PPCInterpreter_t* cpu, uint32 ea)
	{
		if constexpr (Size == 4)
			return Bus::Read32(cpu, ea);
		else if constexpr (Size == 2)
			return Bus::Read16(cpu, ea);
		else
			return Bus::Read8(cpu, ea);
	}

	// remaining integer loads; update forms skip the update when rA = 0 or rA = rD
	enum class LoadExt { Zero, Sign, ByteReverse };

	template<typename Bus, uint32 Size, LoadExt Ext, bool Indexed, bool Update>
	void Op_Load(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 ea = Indexed ? EA_X(cpu, op) : EA_D(cpu, op);
		uint32 value = LoadValue<Bus, Size>(cpu, ea);
		if constexpr (Ext == LoadExt::Sign)
			value = (uint32)(sint32)(sint16)(uint16)value;
		else if constexpr (Ext == LoadExt::ByteReverse)
			value = (Size == 4) ? _swapEndianU32(value) : (uint32)_swapEndianU16((uint16)value);
		uint32 rD = ppcOpField_D(op);
		cpu->gpr[rD] = value;
		if constexpr (Update)
		{
			uint32 rA = ppcOpField_A(op);
			if (rA != 0 && rA != rD)
				cpu->gpr[rA] = ea;
		}
		PPCInterpreter_nextInstruction(cpu);
	}

	template<typename Bus>
	void Op_LMW(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 ea = EA_D(cpu, op);
		for (uint32 r = ppcOpField_D(op); r < 32; r++, ea += 4)
			cpu->gpr[r] = Bus::Read32(cpu, ea);
		PPCInterpreter_nextInstruction(cpu);
	}

	// lswi/lswx: bytes are packed MSB first into consecutive registers
	// (wrapping to r0); the last register is padded with zero bytes
	template<typename Bus>
	void LoadString(PPCInterpreter_t* cpu, uint32 reg, uint32 ea, uint32 byteCount)
	{
		uint32 shiftReg = 0;
		uint32 inReg = 0;
		for (uint32 i = 0; i < byteCount; i++)
		{
			if (inReg == 4)
			{
				cpu->gpr[reg] = shiftReg;
				reg = (reg + 1) & 31;
				shiftReg = 0;
				inReg = 0;
			}
			shiftReg = (shiftReg << 8) | Bus::Read8(cpu, ea + i);
			inReg++;
		}
		cpu->gpr[reg] = shiftReg << (8 * (4 - inReg));
	}

	template<typename Bus>
	void Op_LSWI(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		uint32 count = ppcOpField_B(op);
		LoadString<Bus>(cpu, ppcOpField_D(op), rA ? cpu->gpr[rA] : 0, count == 0 ? 32 : count);
		PPCInterpreter_nextInstruction(cpu);
	}

	template<typename Bus>
	void Op_LSWX(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 count = cpu->spr.XER & 0x7F;
		if (count != 0)
			LoadString<Bus>(cpu, ppcOpField_D(op), EA_X(cpu, op), count);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- floating point loads ----

	// lfs/lfsu/lfsx/lfsux: the widened value goes to both paired single slots
	template<typename Bus, bool Indexed, bool Update>
	void Op_LFS(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 ea = Indexed ? EA_X(cpu, op) : EA_D(cpu, op);
		uint64 value = Bus::ReadSingleWidened(cpu, ea);
		uint32 rA = ppcOpField_A(op);
		if constexpr (Update)
		{
			if (rA != 0)
				cpu->gpr[rA] = ea;
		}
		FPR_t& d = cpu->fpr[ppcOpField_D(op)];
		d.fp0int = value;
		d.fp1int = value;
		PPCInterpreter_nextInstruction(cpu);
	}

	// lfd forms only write ps0
	template<typename Bus, bool Indexed, bool Update>
	void Op_LFD(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 ea = Indexed ? EA_X(cpu, op) : EA_D(cpu, op);
		cpu->fpr[ppcOpField_D(op)].fp0int = Bus::ReadDouble(cpu, ea);
		if constexpr (Update)
		{
			uint32 rA = ppcOpField_A(op);
			if (rA != 0)
				cpu->gpr[rA] = ea;
		}
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- floating point stores ----

	// stfs/stfsu/stfsx/stfsux (stfsux advances the instruction pointer first)
	template<typename Bus, bool Indexed, bool Update>
	void Op_STFS(PPCInterpreter_t* cpu, uint32 op)
	{
		constexpr bool advanceFirst = Indexed && Update;
		if constexpr (advanceFirst)
			PPCInterpreter_nextInstruction(cpu);
		uint32 ea = Indexed ? EA_X(cpu, op) : EA_D(cpu, op);
		Bus::WriteSingleNarrowed(cpu, ea, cpu->fpr[ppcOpField_D(op)].fp0int);
		if constexpr (Update)
		{
			uint32 rA = ppcOpField_A(op);
			if (rA != 0)
				cpu->gpr[rA] = ea;
		}
		if constexpr (!advanceFirst)
			PPCInterpreter_nextInstruction(cpu);
	}

	// stfd forms advance the instruction pointer before the store. The
	// update forms with rA = 0 store to r0 (stfdu) or rB (stfdux) without
	// adding the displacement.
	template<typename Bus, bool Indexed, bool Update>
	void Op_STFD(PPCInterpreter_t* cpu, uint32 op)
	{
		PPCInterpreter_nextInstruction(cpu);
		uint32 rA = ppcOpField_A(op);
		uint32 ea;
		if constexpr (Update)
		{
			if (rA != 0)
			{
				ea = Indexed ? EA_X(cpu, op) : EA_D(cpu, op);
				cpu->gpr[rA] = ea;
			}
			else
				ea = Indexed ? cpu->gpr[ppcOpField_B(op)] : cpu->gpr[0];
		}
		else
			ea = Indexed ? EA_X(cpu, op) : EA_D(cpu, op);
		Bus::WriteDouble(cpu, ea, cpu->fpr[ppcOpField_D(op)].fp0int);
	}

	template<typename Bus>
	void Op_STFIWX(PPCInterpreter_t* cpu, uint32 op)
	{
		Bus::Write32(cpu, EA_X(cpu, op), (uint32)cpu->fpr[ppcOpField_D(op)].fp0int);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- paired single quantized load/store ----
	//
	// D form: 12-bit signed displacement, W at bit 16, I (GQR index) at bits 17-19
	// X form (opcode 4): W at bit 21, I at bits 22-24; the update X forms
	// (psq_lux/psq_stux) share the extended opcode bits of psq_lx/psq_stx and
	// execute as those.

	struct PsqFields
	{
		uint32 ea;
		bool singleSlot; // W bit
		uint32 gqr;
	};

	inline uint32 PsqDisplacement(uint32 op)
	{
		return (uint32)(((sint32)(op << 20)) >> 20);
	}

	inline PsqFields DecodePsqD(PPCInterpreter_t* cpu, uint32 op, bool update)
	{
		PsqFields f;
		uint32 rA = ppcOpField_A(op);
		f.ea = (rA ? cpu->gpr[rA] : 0) + PsqDisplacement(op);
		if (update && rA != 0)
			cpu->gpr[rA] = f.ea;
		f.singleSlot = (op & 0x8000) != 0;
		f.gqr = cpu->spr.UGQR[(op >> 12) & 7];
		return f;
	}

	inline PsqFields DecodePsqX(PPCInterpreter_t* cpu, uint32 op)
	{
		PsqFields f;
		f.ea = EA_X(cpu, op);
		f.singleSlot = (op & 0x400) != 0;
		f.gqr = cpu->spr.UGQR[(op >> 7) & 7];
		return f;
	}

	template<typename Bus>
	uint32 PsqReadRaw(PPCInterpreter_t* cpu, uint32 ea, uint32 size)
	{
		if (size == 1)
			return Bus::Read8(cpu, ea);
		if (size == 2)
			return Bus::Read16(cpu, ea);
		return Bus::Read32(cpu, ea);
	}

	template<typename Bus>
	void PsqWriteRaw(PPCInterpreter_t* cpu, uint32 ea, uint32 size, uint32 value)
	{
		if (size == 1)
			Bus::Write8(cpu, ea, (uint8)value);
		else if (size == 2)
			Bus::Write16(cpu, ea, (uint16)value);
		else
			Bus::Write32(cpu, ea, value);
	}

	template<typename Bus>
	void PsqLoad(PPCInterpreter_t* cpu, uint32 frD, const PsqFields& f)
	{
		uint32 type = (f.gqr >> 16) & 7;
		uint32 scale = (f.gqr >> 24) & 0x3F;
		uint32 size = GqrAccessSize(type);
		uint32 raw0 = PsqReadRaw<Bus>(cpu, f.ea, size);
		uint32 raw1 = f.singleSlot ? 0 : PsqReadRaw<Bus>(cpu, f.ea + size, size);
		FPR_t& d = cpu->fpr[frD];
		d.fp0 = (double)Dequantize(raw0, type, scale);
		d.fp1 = f.singleSlot ? 1.0 : (double)Dequantize(raw1, type, scale);
	}

	template<typename Bus>
	void PsqStore(PPCInterpreter_t* cpu, uint32 frS, const PsqFields& f)
	{
		uint32 type = f.gqr & 7;
		uint32 scale = (f.gqr >> 8) & 0x3F;
		uint32 size = GqrAccessSize(type);
		const FPR_t& s = cpu->fpr[frS];
		PsqWriteRaw<Bus>(cpu, f.ea, size, Quantize((float)s.fp0, type, scale));
		if (!f.singleSlot)
			PsqWriteRaw<Bus>(cpu, f.ea + size, size, Quantize((float)s.fp1, type, scale));
	}

	// psq_l and psq_lu advance the instruction pointer before loading
	template<typename Bus, bool Update>
	void Op_PSQ_L(PPCInterpreter_t* cpu, uint32 op)
	{
		PPCInterpreter_nextInstruction(cpu);
		PsqFields f = DecodePsqD(cpu, op, Update);
		PsqLoad<Bus>(cpu, ppcOpField_D(op), f);
	}

	template<typename Bus>
	void Op_PSQ_LX(PPCInterpreter_t* cpu, uint32 op)
	{
		PPCInterpreter_nextInstruction(cpu);
		PsqFields f = DecodePsqX(cpu, op);
		PsqLoad<Bus>(cpu, ppcOpField_D(op), f);
	}

	template<typename Bus, bool Update>
	void Op_PSQ_ST(PPCInterpreter_t* cpu, uint32 op)
	{
		PsqFields f = DecodePsqD(cpu, op, Update);
		PsqStore<Bus>(cpu, ppcOpField_D(op), f);
		PPCInterpreter_nextInstruction(cpu);
	}

	template<typename Bus>
	void Op_PSQ_STX(PPCInterpreter_t* cpu, uint32 op)
	{
		PPCInterpreter_nextInstruction(cpu);
		PsqFields f = DecodePsqX(cpu, op);
		PsqStore<Bus>(cpu, ppcOpField_D(op), f);
	}

	// ---- cache line zero ----

	template<typename Bus>
	void Op_DCBZ(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 line = EA_X(cpu, op) & ~31u;
		if (!Bus::ZeroCacheLine(cpu, line))
			return; // LLE: interrupted by a data storage interrupt
		PPCInterpreter_nextInstruction(cpu);
	}
}
