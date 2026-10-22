// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"

// The GS copy program runs after the geometry shader: it reads every vertex the geometry
// shader wrote to the GS->VS ring and exports it as a regular vertex shader would. PixelU
// does not run it; it only extracts which ring offset ends up in which export (and which
// transform feedback buffer), so the geometry shader can export directly.

namespace
{
	constexpr uint8 kNotExported = 0xFF;
	constexpr uint32 kRingBufferId = 0x9F; // fetch buffer id of the GS->VS ring

	struct CFWord
	{
		uint32 w0;
		uint32 w1;

		uint32 opcode() const { return (w1 >> 23) & 0x7F; }
		bool endOfProgram() const { return ((w1 >> 21) & 1) != 0; }
		uint32 clauseAddr() const { return w0; } // in units of 8 bytes
		uint32 clauseCount() const { return (((w1 >> 10) & 7) | (((w1 >> 19) & 1) << 3)) + 1; }
		// export / memory write fields
		uint32 arrayBase() const { return w0 & 0x1FFF; }
		uint32 exportType() const { return (w0 >> 13) & 3; }
		uint32 sourceGPR() const { return (w0 >> 15) & 0x7F; }
		uint32 burstCount() const { return (w1 >> 17) & 0xF; }
		uint32 swizzle(uint32 c) const { return (w1 >> (c * 3)) & 7; }
		uint32 memArraySize() const { return w1 & 0xFFF; }
		uint32 memCompMask() const { return (w1 >> 12) & 0xF; }
	};

	// most recent mapping entry of a register (a register can be loaded more than once)
	sint32 LatestEntryOfGPR(const LatteParsedGSCopyShader* copy, uint32 gpr)
	{
		for (sint32 i = copy->numParam - 1; i >= 0; i--)
		{
			if (copy->paramMapping[i].gprIndex == gpr)
				return i;
		}
		return -1;
	}

	void RecordRingLoad(LatteParsedGSCopyShader* copy, uint32 ringOffset, uint32 gpr)
	{
		if (copy->numParam >= GPU7_COPY_SHADER_MAX_PARAMS)
		{
			cemu_assert_suspicious();
			return;
		}
		auto& entry = copy->paramMapping[copy->numParam++];
		entry.offset = (uint16)ringOffset;
		entry.gprIndex = (uint16)gpr;
		entry.exportParam = kNotExported;
	}

	void RecordExport(LatteParsedGSCopyShader* copy, uint32 gpr, uint32 exportType, uint32 exportParam)
	{
		const sint32 i = LatestEntryOfGPR(copy, gpr);
		if (i < 0)
		{
			cemu_assert_debug(false); // exported register was never loaded
			return;
		}
		cemu_assert_debug(copy->paramMapping[i].exportParam == kNotExported && exportParam < 0x100);
		copy->paramMapping[i].exportType = (uint8)exportType;
		copy->paramMapping[i].exportParam = (uint8)exportParam;
	}

	void RecordStreamWrite(LatteParsedGSCopyShader* copy, uint32 bufferIndex, const CFWord& cf)
	{
		const sint32 i = LatestEntryOfGPR(copy, cf.sourceGPR());
		if (i < 0)
		{
			cemu_assert_debug(false);
			return;
		}
		LatteGSCopyShaderStreamWrite_t write;
		write.bufferIndex = (uint8)bufferIndex;
		write.offset = copy->paramMapping[i].offset;
		write.exportArrayBase = cf.arrayBase();
		write.memWriteArraySize = cf.memArraySize();
		write.memWriteCompMask = cf.memCompMask();
		copy->list_streamWrites.push_back(write);
	}

	// a VTX clause of ring fetches; stops at the first instruction that is something else
	void ParseFetchClause(LatteParsedGSCopyShader* copy, const uint8* program, uint32 programSize, const CFWord& cf)
	{
		const uint32 count = cf.clauseCount();
		for (uint32 n = 0; n < count; n++)
		{
			// fetch instructions are 16 bytes, the clause address counts 8 byte units
			const uint64 byteOffset = (uint64)cf.clauseAddr() * 8 + (uint64)n * 16;
			if (byteOffset + 16 > programSize)
				return;
			uint32 words[4];
			memcpy(words, program + byteOffset, sizeof(words));
			if ((words[0] & 0x1F) != GPU7_TEX_INST_VFETCH)
				return;
			if (((words[0] >> 8) & 0xFF) != kRingBufferId)
				return;
			RecordRingLoad(copy, words[2] & 0xFFFF, words[1] & 0x7F);
		}
	}

	void ParseExport(LatteParsedGSCopyShader* copy, const CFWord& cf)
	{
		bool allZero = true, identity = true;
		for (uint32 c = 0; c < 4; c++)
		{
			allZero = allZero && cf.swizzle(c) == 4;
			identity = identity && cf.swizzle(c) == c;
		}
		// position written as constant 0 when the game never set it
		if (cf.arrayBase() == GPU7_DECOMPILER_CF_EXPORT_BASE_POSITION && allZero)
			return;
		if (!identity)
		{
			cemu_assert_debug(false); // swizzled exports are not expected in copy programs
			return;
		}
		for (uint32 b = 0; b <= cf.burstCount(); b++)
			RecordExport(copy, cf.sourceGPR() + b, cf.exportType(), cf.arrayBase() + b);
	}
}

LatteParsedGSCopyShader* LatteGSCopyShaderParser_parse(uint8* programData, uint32 programSize)
{
	cemu_assert_debug((programSize & 3) == 0);
	LatteParsedGSCopyShader* copy = new LatteParsedGSCopyShader();
	copy->numParam = 0;
	const uint32 cfCount = programSize / 8;
	for (uint32 i = 0; i < cfCount; i++)
	{
		CFWord cf;
		memcpy(&cf.w0, programData + i * 8, 4);
		memcpy(&cf.w1, programData + i * 8 + 4, 4);
		const uint32 op = cf.opcode();
		if (op >= 0x40)
		{
			// ALU clause encoding, copy programs never contain one
			cemu_assert_debug(false);
			delete copy;
			return nullptr;
		}
		switch (op)
		{
		case GPU7_CF_INST_NOP:
		case GPU7_CF_INST_CALL_FS:
			break;
		case GPU7_CF_INST_VTX:
			ParseFetchClause(copy, programData, programSize, cf);
			break;
		case GPU7_CF_INST_EXPORT:
		case GPU7_CF_INST_EXPORT_DONE:
			ParseExport(copy, cf);
			break;
		case GPU7_CF_INST_MEM_STREAM0_WRITE:
			RecordStreamWrite(copy, 0, cf);
			break;
		case GPU7_CF_INST_MEM_STREAM1_WRITE:
			RecordStreamWrite(copy, 1, cf);
			break;
		default:
			cemuLog_log(LogType::Force, "GS copy program: unhandled CF opcode 0x{:x}", op);
			break;
		}
		if (cf.endOfProgram())
			break;
	}
	return copy;
}

bool LatteGSCopyShaderParser_getExportTypeByOffset(LatteParsedGSCopyShader* shaderContext, uint32 offset, uint32* exportType, uint32* exportParam)
{
	for (sint32 i = 0; i < shaderContext->numParam; i++)
	{
		if (shaderContext->paramMapping[i].offset != offset)
			continue;
		*exportType = shaderContext->paramMapping[i].exportType;
		*exportParam = shaderContext->paramMapping[i].exportParam;
		return true;
	}
	return false;
}
