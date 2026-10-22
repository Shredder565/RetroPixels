// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// x86-64 backend: floating point loads and stores, including the quantized
// paired single accesses.

#include "Cafe/HW/Espresso/PPCState.h"
#include "BackendX64.h"
#include "X64Emit.hpp"

using namespace x64;

namespace X64Backend
{
	bool CodeGenerator::EmitFloatMemory(const IMLInstruction& inst)
	{
		auto& e = m_e;
		const Xmm s0 = kXmmScratch0;
		const Xmm s1 = kXmmScratch1;
		switch (inst.op)
		{
		case IMLOp::LOADFS:
		{
			// exact widening (lfs keeps signaling NaNs signaling, unlike CVTSS2SD)
			Mem m = GuestAddress(inst);
			e.MOV_rm(RCX, m);
			e.BSWAP(RCX);
			e.MOV_rr(RAX, RCX);
			e.ALU_ri(ALU_AND, RAX, 0x7F800000);
			e.ALU_ri(ALU_CMP, RAX, 0x7F800000);
			size_t special = e.Jcc_rel32(CC_E);
			e.MOVD_xr(s0, RCX);
			e.CVTSS2SD(s0, s0);
			size_t done = e.JMP_rel32();
			e.BindRel32(special, e.Size());
			// infinity or NaN: sign, all ones exponent, fraction moved to the top
			e.MOV_rr(RAX, RCX);
			e.ALU_ri(ALU_AND, RAX, 0x007FFFFF);
			e.SHIFT_ri(SH_SHL, RAX, 29, true);
			e.MOV_rr(RDX, RCX);
			e.ALU_ri(ALU_AND, RDX, 0x80000000);
			e.SHIFT_ri(SH_SHL, RDX, 32, true);
			e.ALU_rr(ALU_OR, RAX, RDX, true);
			e.MOV_ri64(RDX, 0x7FF0000000000000ull);
			e.ALU_rr(ALU_OR, RAX, RDX, true);
			e.MOVQ_xr(s0, RAX);
			e.BindRel32(done, e.Size());
			e.UNPCKLPD(s0, s0);
			e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		}
		case IMLOp::LOADFD:
		{
			Mem m = GuestAddress(inst);
			e.MOV_rm(RCX, m, true);
			e.BSWAP(RCX, true);
			e.MOVQ_xr(s0, RCX);
			e.MOVSD_rr(Fpr(inst.r[0]), s0);
			return true;
		}
		case IMLOp::STOREF:
		{
			Xmm src = Fpr(inst.r[0]);
			switch ((IMLFMem)inst.sub)
			{
			case IMLFMem::SINGLE:
			{
				// stfs: no rounding. Values in the single denormal range are
				// denormalised, everything else keeps the sign, the top
				// exponent bit and the next 30 bits
				e.MOVQ_rx(RAX, src);
				e.MOV_rr(RCX, RAX, true);
				e.SHIFT_ri(SH_SHR, RCX, 52, true);
				e.ALU_ri(ALU_AND, RCX, 0x7FF);
				e.LEA32(RDX, mem(RCX, -874));
				e.ALU_ri(ALU_CMP, RDX, 22);
				size_t denormal = e.Jcc_rel32(CC_BE);
				e.MOV_rr(RCX, RAX, true);
				e.SHIFT_ri(SH_SHR, RCX, 32, true);
				e.ALU_ri(ALU_AND, RCX, 0xC0000000);
				e.SHIFT_ri(SH_SHR, RAX, 29, true);
				e.ALU_ri(ALU_AND, RAX, 0x3FFFFFFF);
				e.ALU_rr(ALU_OR, RCX, RAX);
				size_t done = e.JMP_rel32();
				e.BindRel32(denormal, e.Size());
				e.NEG(RCX);
				e.ALU_ri(ALU_ADD, RCX, 905);
				e.MOV_rr(RDX, RAX, true);
				e.SHIFT_ri(SH_SHL, RDX, 12, true);
				e.SHIFT_ri(SH_SHR, RDX, 33, true);
				e.ALU_ri(ALU_OR, RDX, 0x80000000);
				e.SHIFT_rcl(SH_SHR, RDX);
				e.SHIFT_ri(SH_SHR, RAX, 32, true);
				e.ALU_ri(ALU_AND, RAX, 0x80000000);
				e.ALU_rr(ALU_OR, RDX, RAX);
				e.MOV_rr(RCX, RDX);
				e.BindRel32(done, e.Size());
				e.BSWAP(RCX);
				Mem m = GuestAddress(inst);
				e.MOV_mr(m, RCX);
				return true;
			}
			case IMLFMem::DOUBLE:
			{
				e.MOVQ_rx(RCX, src);
				e.BSWAP(RCX, true);
				Mem m = GuestAddress(inst);
				e.MOV_mr(m, RCX, true);
				return true;
			}
			case IMLFMem::IWX:
			{
				e.MOVD_rx(RCX, src);
				e.BSWAP(RCX);
				Mem m = GuestAddress(inst);
				e.MOV_mr(m, RCX);
				return true;
			}
			}
			return false;
		}
		case IMLOp::PSQLOAD:
		{
			bool single = (inst.flags & IMLF_PSQ_W) != 0;
			GuestAddressToEAX(inst);
			uint8 gqr = Gpr(inst.r[3]);
			Xmm d = Fpr(inst.r[0]);
			// float data (load type 0) is converted inline
			e.TEST_ri(gqr, 0x00070000);
			size_t slow = e.Jcc_rel32(CC_NE);
			if (single)
			{
				e.MOV_rm(RCX, mem(kRegMemBase, RAX, 1, 0));
				e.BSWAP(RCX);
				e.MOVD_xr(s0, RCX);
				e.CVTSS2SD(s0, s0);
				LoadConstantTable();
				e.MOVUPD_rm(s1, mem(RAX, offsetof(VectorConstants, one)));
				e.MOVSD_rr(s1, s0);
				e.MOVAPD(d, s1);
			}
			else
			{
				e.MOV_rm(RCX, mem(kRegMemBase, RAX, 1, 0), true);
				e.BSWAP(RCX, true);
				e.SHIFT_ri(SH_ROL, RCX, 32, true); // first value in the low half
				e.MOVQ_xr(s0, RCX);
				e.CVTPS2PD(s0, s0);
				e.MOVAPD(d, s0);
			}
			size_t done = e.JMP_rel32();
			e.BindRel32(slow, e.Size());
			e.MOV_mr(CPU(offsetof(PPCInterpreter_t, temporaryGPR)), RAX);
			e.MOV_mr(CPU(offsetof(PPCInterpreter_t, temporaryGPR) + 4), gqr);
			CallHelper(single ? (const void*)&Helper_PSQLoadSingle : (const void*)&Helper_PSQLoad, inst.saveMask);
			e.MOVUPD_rm(d, CPU(offsetof(PPCInterpreter_t, temporaryFPR)));
			e.BindRel32(done, e.Size());
			return true;
		}
		case IMLOp::PSQSTORE:
		{
			bool single = (inst.flags & IMLF_PSQ_W) != 0;
			GuestAddressToEAX(inst);
			uint8 gqr = Gpr(inst.r[3]);
			Xmm src = Fpr(inst.r[0]);
			// float data (store type 0) is converted inline
			e.TEST_ri(gqr, 0x00000007);
			size_t slow = e.Jcc_rel32(CC_NE);
			if (single)
			{
				e.CVTSD2SS(s0, src);
				e.MOVD_rx(RCX, s0);
				e.BSWAP(RCX);
				e.MOV_mr(mem(kRegMemBase, RAX, 1, 0), RCX);
			}
			else
			{
				e.CVTPD2PS(s0, src);
				e.MOVQ_rx(RCX, s0);
				e.SHIFT_ri(SH_ROL, RCX, 32, true);
				e.BSWAP(RCX, true);
				e.MOV_mr(mem(kRegMemBase, RAX, 1, 0), RCX, true);
			}
			size_t done = e.JMP_rel32();
			e.BindRel32(slow, e.Size());
			e.MOV_mr(CPU(offsetof(PPCInterpreter_t, temporaryGPR)), RAX);
			e.MOV_mr(CPU(offsetof(PPCInterpreter_t, temporaryGPR) + 4), gqr);
			e.MOVUPD_mr(CPU(offsetof(PPCInterpreter_t, temporaryFPR)), src);
			CallHelper(single ? (const void*)&Helper_PSQStoreSingle : (const void*)&Helper_PSQStore, inst.saveMask);
			e.BindRel32(done, e.Size());
			return true;
		}
		default:
			return false;
		}
	}
}
