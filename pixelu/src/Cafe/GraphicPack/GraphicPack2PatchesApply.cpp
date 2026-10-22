// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Common/FileStream.h"
#include "Cemu/PPCAssembler/ppcAssembler.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/Debugger/DebugSymbolStorage.h"

namespace
{
	// The expression parser calls back through plain function pointers, so the
	// group and line being resolved are kept here while an expression is evaluated.
	struct ResolveSession
	{
		PatchContext_t* context{};
		PatchGroup* group{};
		sint32 line{-1};
		bool missingSymbol{};
		bool reportMissing{}; // only the final diagnostic pass records missing names
	};
	ResolveSession s_session;

	void NoteMissing(std::string name)
	{
		s_session.missingSymbol = true;
		if (s_session.reportMissing)
			s_session.context->unresolvedSymbols.emplace(s_session.line, s_session.group, std::move(name));
	}

	// patch addresses below the code cave limit are offsets into the group's cave,
	// everything else must fall into a section of the module being patched
	bool MapPatchAddress(PatchGroup* group, const PatchContext_t& ctx, uint32 address, uint32& mapped)
	{
		if (address <= PatchGroup::CODECAVE_MAX_OFFSET)
		{
			mapped = group->getCodeCaveBase() + address;
			return true;
		}
		const RPLModule* module = ctx.matchedModule;
		for (size_t i = 0; i < module->sectionTable.size(); i++)
		{
			const RPLFileSectionEntry& section = module->sectionTable[i];
			const uint32 start = section.virtualAddress;
			if (address >= start && address < start + (uint32)section.sectionSize)
			{
				mapped = memory_getVirtualOffsetFromPointer(module->sectionAddressTable[i].ptr) + (address - start);
				return true;
			}
		}
		mapped = 0;
		return false;
	}

	double LookupImport(std::string_view importName)
	{
		const size_t dot = importName.find('.');
		const char* problem = " (invalid import syntax)";
		if (dot != std::string_view::npos)
		{
			const std::string module(importName.substr(0, dot));
			const std::string function(importName.substr(dot + 1));
			const uint32 handle = RPLLoader_GetHandleByModuleName(module.c_str());
			if (handle == RPL_INVALID_HANDLE)
				problem = " (module not found)";
			else if (MPTR address = RPLLoader_FindModuleOrHLEExport(handle, false, function.c_str()))
				return (double)address;
			else
				problem = " (function not found)";
		}
		NoteMissing(std::string(importName) + problem);
		return 0.0;
	}

	// names inside patch expressions: $preset variables, import.<module>.<function> and patch symbols
	double ResolveName(std::string_view name)
	{
		if (!name.empty() && name.front() == '$')
		{
			double value;
			if (s_session.context->graphicPack->ResolvePresetConstant(std::string(name), value))
				return value;
			NoteMissing(std::string(name));
			return 0.0;
		}
		if (name.size() >= 7 && boost::iequals(name.substr(0, 7), "import."))
			return LookupImport(name.substr(7));
		auto it = s_session.context->map_values.find(std::string(name));
		if (it == s_session.context->map_values.end())
		{
			NoteMissing(std::string(name));
			return 0.0;
		}
		return it->second;
	}

	double ApplyFunction(std::string_view name, double input)
	{
		std::string lower(name);
		for (char& c : lower)
			c = (char)tolower((unsigned char)c);
		if (lower == "ha" || lower == "ha16")
			return TExpressionParser<double>::ExpressionFuncHA(input);
		if (lower == "hi" || lower == "hi16")
			return TExpressionParser<double>::ExpressionFuncHI(input);
		if (lower == "lo" || lower == "lo16")
			return TExpressionParser<double>::ExpressionFuncLO(input);
		if (lower == "reloc")
		{
			const uint32 address = (uint32)input;
			uint32 mapped;
			if (MapPatchAddress(s_session.group, *s_session.context, address, mapped))
				return (double)mapped;
			s_session.context->errorHandler.printError(s_session.group, s_session.line, fmt::format("reloc({0:#08x}): address is not inside the code cave or a module section", address));
			return 0.0;
		}
		NoteMissing(std::string(name) + "() (unknown function)");
		return 0.0;
	}

	template<typename T>
	EXPRESSION_RESOLVE_RESULT Evaluate(PatchContext_t& ctx, const std::string& expression, T& result, sint32 line)
	{
		s_session.line = line;
		s_session.missingSymbol = false;
		ExpressionParser parser;
		parser.AddConstantCallback(ResolveName);
		parser.SetFunctionCallback(ApplyFunction);
		try
		{
			result = (T)parser.Evaluate(expression);
		}
		catch (const std::exception&)
		{
			ctx.errorHandler.printError(nullptr, -1, fmt::format("Expression \"{}\" could not be evaluated", expression));
			return EXPRESSION_RESOLVE_RESULT::EXPRESSION_ERROR;
		}
		return s_session.missingSymbol ? EXPRESSION_RESOLVE_RESULT::UNKNOWN_VARIABLE : EXPRESSION_RESOLVE_RESULT::AVAILABLE;
	}

	PATCH_RESOLVE_RESULT ToPatchResult(EXPRESSION_RESOLVE_RESULT r)
	{
		switch (r)
		{
		case EXPRESSION_RESOLVE_RESULT::AVAILABLE:
			return PATCH_RESOLVE_RESULT::RESOLVED;
		case EXPRESSION_RESOLVE_RESULT::UNKNOWN_VARIABLE:
			return PATCH_RESOLVE_RESULT::UNKNOWN_VARIABLE;
		default:
			return PATCH_RESOLVE_RESULT::EXPRESSION_ERROR;
		}
	}

	// makes the name known to later expressions; false if it already exists
	bool DefineSymbol(PatchContext_t& ctx, const std::string& name, uint32 value)
	{
		cemuLog_log(LogType::Patches, "Patch symbol {} = 0x{:08x}", name, value);
		if (!ctx.map_values.try_emplace(name, value).second)
			return false;
		rplSymbolStorage_store(ctx.graphicPack->GetName().data(), name.data(), value, RPL_STORED_SYMBOL_PATCH);
		return true;
	}

	template<typename T>
	void StoreBE(std::vector<uint8>& buffer, uint32 offset, T value)
	{
		cemu_assert(offset + sizeof(T) <= buffer.size());
		*(betype<T>*)(buffer.data() + offset) = value;
	}

	uint32 LoadBE32(const std::vector<uint8>& buffer, uint32 offset)
	{
		cemu_assert(offset + sizeof(uint32) <= buffer.size());
		return *(const betype<uint32>*)(buffer.data() + offset);
	}

	// relative branch displacement from 'from' to 'to'; false when it does not fit
	// (backward reach is one step larger than forward reach, as with two's complement)
	bool BranchDisplacement(uint32 from, uint32 to, uint32 reach, uint32& displacement)
	{
		if (to < from)
		{
			const uint32 back = from - to;
			if (back > reach)
				return false;
			displacement = 0u - back;
			return true;
		}
		const uint32 forward = to - from;
		if (forward >= reach)
			return false;
		displacement = forward;
		return true;
	}

	struct PendingEntries
	{
		PatchGroup* group;
		std::vector<PatchEntry*> entries;
	};

	// one pass over everything not resolved yet; returns how many entries got resolved
	sint32 ResolvePass(PatchContext_t& ctx, std::vector<PendingEntries>& pending, bool reportMissing)
	{
		s_session.reportMissing = reportMissing;
		sint32 resolvedCount = 0;
		for (PendingEntries& p : pending)
		{
			s_session.group = p.group;
			std::erase_if(p.entries, [&](PatchEntry* entry) {
				const PATCH_RESOLVE_RESULT r = entry->resolve(ctx);
				switch (r)
				{
				case PATCH_RESOLVE_RESULT::RESOLVED:
					resolvedCount++;
					return true;
				case PATCH_RESOLVE_RESULT::UNKNOWN_VARIABLE: // may work on a later pass
				case PATCH_RESOLVE_RESULT::INVALID_ADDRESS: // already reported
				case PATCH_RESOLVE_RESULT::VARIABLE_CONFLICT: // already reported
					return false;
				default:
					ctx.errorHandler.printError(p.group, entry->getLineNumber(), "Expression could not be evaluated or the value is unusable");
					return false;
				}
			});
		}
		return resolvedCount;
	}

	bool AllResolved(const std::vector<PendingEntries>& pending)
	{
		return std::all_of(pending.begin(), pending.end(), [](const PendingEntries& p) { return p.entries.empty(); });
	}
}

// ---------------------------------------------------------------- entries

PATCH_RESOLVE_RESULT PatchEntryInstruction::resolveReloc(PatchContext_t& ctx, PPCAssemblerReloc* reloc)
{
	const uint32 offset = reloc->m_byteOffset;
	const MPTR target = m_finalAddress + offset;

	if (reloc->m_relocType == PPCASM_RELOC::FLOAT)
	{
		float value;
		auto r = Evaluate<float>(ctx, reloc->m_expression, value, m_line);
		if (r != EXPRESSION_RESOLVE_RESULT::AVAILABLE)
			return ToPatchResult(r);
		StoreBE<float>(m_final, offset, value);
		DebugSymbolStorage::StoreDataType(target, DEBUG_SYMBOL_TYPE::FLOAT);
		return PATCH_RESOLVE_RESULT::RESOLVED;
	}
	if (reloc->m_relocType == PPCASM_RELOC::DOUBLE)
	{
		double value;
		auto r = Evaluate<double>(ctx, reloc->m_expression, value, m_line);
		if (r != EXPRESSION_RESOLVE_RESULT::AVAILABLE)
			return ToPatchResult(r);
		StoreBE<double>(m_final, offset, value);
		DebugSymbolStorage::StoreDataType(target, DEBUG_SYMBOL_TYPE::DOUBLE);
		return PATCH_RESOLVE_RESULT::RESOLVED;
	}

	uint32 value;
	auto r = Evaluate<uint32>(ctx, reloc->m_expression, value, m_line);
	if (r != EXPRESSION_RESOLVE_RESULT::AVAILABLE)
		return ToPatchResult(r);

	auto branchTooFar = [&]() {
		ctx.errorHandler.printError(nullptr, m_line, fmt::format("Branch target '{}' is out of range (use mtctr + bctr to jump further)", reloc->m_expression));
		return PATCH_RESOLVE_RESULT::VALUE_ERROR;
	};

	switch (reloc->m_relocType)
	{
	case PPCASM_RELOC::U32:
		StoreBE<uint32>(m_final, offset, value);
		DebugSymbolStorage::StoreDataType(target, DEBUG_SYMBOL_TYPE::U32);
		return PATCH_RESOLVE_RESULT::RESOLVED;
	case PPCASM_RELOC::U16:
		StoreBE<uint16>(m_final, offset, (uint16)value);
		DebugSymbolStorage::StoreDataType(target, DEBUG_SYMBOL_TYPE::U16);
		return PATCH_RESOLVE_RESULT::RESOLVED;
	case PPCASM_RELOC::U8:
		StoreBE<uint8>(m_final, offset, (uint8)value);
		DebugSymbolStorage::StoreDataType(target, DEBUG_SYMBOL_TYPE::U8);
		return PATCH_RESOLVE_RESULT::RESOLVED;
	case PPCASM_RELOC::U32_MASKED_IMM:
	{
		uint32 word = LoadBE32(m_final, offset);
		cemu_assert_debug(reloc->m_bitCount != 0);
		const uint32 field = (0xFFFFFFFFu >> (32 - reloc->m_bitCount)) << reloc->m_bitOffset;
		word = (word & ~field) | ((value << reloc->m_bitOffset) & field);
		StoreBE<uint32>(m_final, offset, word);
		return PATCH_RESOLVE_RESULT::RESOLVED;
	}
	case PPCASM_RELOC::BRANCH_S26:
	{
		constexpr uint32 kField = 0x03FFFFFC;
		uint32 word = LoadBE32(m_final, offset);
		uint32 encoded;
		if (word & 2) // AA bit: absolute target
		{
			if (value >= kField)
			{
				cemuLog_log(LogType::Force, "Patches: absolute branch target '{}' on line {} is out of range", reloc->m_expression, m_line);
				return PATCH_RESOLVE_RESULT::VALUE_ERROR;
			}
			encoded = value;
		}
		else if (!BranchDisplacement(getRelocatedAddr() + offset, value, 0x1FFFFFF, encoded))
			return branchTooFar();
		word = (word & ~kField) | (encoded & kField);
		StoreBE<uint32>(m_final, offset, word);
		return PATCH_RESOLVE_RESULT::RESOLVED;
	}
	case PPCASM_RELOC::BRANCH_S16:
	{
		constexpr uint32 kField = 0xFFFC;
		uint32 word = LoadBE32(m_final, offset);
		uint32 encoded;
		if (!BranchDisplacement(getRelocatedAddr() + offset, value, 0x8000, encoded))
			return branchTooFar();
		word = (word & ~kField) | (encoded & kField);
		StoreBE<uint32>(m_final, offset, word);
		return PATCH_RESOLVE_RESULT::RESOLVED;
	}
	default:
		cemu_assert_debug(false);
		return PATCH_RESOLVE_RESULT::UNDEFINED_ERROR;
	}
}

PATCH_RESOLVE_RESULT PatchEntryInstruction::resolve(PatchContext_t& ctx)
{
	if (!m_hasFinalAddress)
	{
		if (!MapPatchAddress(s_session.group, ctx, m_patchAddress, m_finalAddress))
		{
			cemuLog_log(LogType::Force, "Patches: address 0x{:08x} on line {} is outside the code cave and the module", m_patchAddress, m_line);
			return PATCH_RESOLVE_RESULT::INVALID_ADDRESS;
		}
		m_hasFinalAddress = true;
	}
	for (PPCAssemblerReloc& reloc : m_relocs)
	{
		if (reloc.isApplied())
			continue;
		const PATCH_RESOLVE_RESULT r = resolveReloc(ctx, &reloc);
		if (r != PATCH_RESOLVE_RESULT::RESOLVED)
			return r;
		reloc.setApplied();
	}
	return PATCH_RESOLVE_RESULT::RESOLVED;
}

void PatchEntryInstruction::applyPatch()
{
	const uint32 address = m_finalAddress;
	if (address == 0)
	{
		cemu_assert_debug(false);
		return;
	}
	uint8* memory = (uint8*)memory_base + address;
	const size_t length = m_final.size();
	if (length != 0) // directives like .align can produce no bytes
	{
		std::memcpy(m_saved.data(), memory, length);
		std::memcpy(memory, m_final.data(), length);
	}
	PPCRecompiler_invalidateRange(address, address + (uint32)length);
}

void PatchEntryInstruction::undoPatch()
{
	const uint32 address = m_finalAddress;
	if (address == 0)
	{
		cemu_assert_debug(false);
		return;
	}
	const uint32 length = (uint32)m_saved.size();
	if (length != 0)
		std::memcpy((uint8*)memory_base + address, m_saved.data(), length);
	PPCRecompiler_invalidateRange(address, address + length);
	rplSymbolStorage_removeRange(address, (sint32)length, RPL_STORED_SYMBOL_PATCH);
	DebugSymbolStorage::ClearRange(address, length);
}

PATCH_RESOLVE_RESULT PatchEntryCemuhookSymbolValue::resolve(PatchContext_t& ctx)
{
	uint32 address;
	auto r = Evaluate<uint32>(ctx, m_expression, address, m_line);
	if (r != EXPRESSION_RESOLVE_RESULT::AVAILABLE)
		return ToPatchResult(r);
	if (!MapPatchAddress(s_session.group, ctx, address, m_address))
		return PATCH_RESOLVE_RESULT::INVALID_ADDRESS;
	if (!DefineSymbol(ctx, m_name, m_address))
	{
		if (s_session.reportMissing)
			ctx.errorHandler.printError(s_session.group, m_line, fmt::format("Symbol {} is defined twice", m_name));
		return PATCH_RESOLVE_RESULT::VARIABLE_CONFLICT;
	}
	return PATCH_RESOLVE_RESULT::RESOLVED;
}

PATCH_RESOLVE_RESULT PatchEntryLabel::resolve(PatchContext_t& ctx)
{
	if (!MapPatchAddress(s_session.group, ctx, m_patchAddress, m_finalAddress))
	{
		if (s_session.reportMissing)
			ctx.errorHandler.printError(s_session.group, m_line, fmt::format("Label {} at {:#08x} is outside the code cave and the module", m_name, m_patchAddress));
		return PATCH_RESOLVE_RESULT::INVALID_ADDRESS;
	}
	if (!DefineSymbol(ctx, m_name, m_finalAddress))
	{
		if (s_session.reportMissing)
			ctx.errorHandler.printError(s_session.group, m_line, fmt::format("Label {} is defined twice", m_name));
		return PATCH_RESOLVE_RESULT::VARIABLE_CONFLICT;
	}
	return PATCH_RESOLVE_RESULT::RESOLVED;
}

PATCH_RESOLVE_RESULT PatchEntryVariableValue::resolve(PatchContext_t& ctx)
{
	uint32 value;
	auto r = Evaluate<uint32>(ctx, m_expression, value, m_line);
	if (r != EXPRESSION_RESOLVE_RESULT::AVAILABLE)
		return ToPatchResult(r);
	if (!DefineSymbol(ctx, m_name, value))
	{
		if (s_session.reportMissing)
			ctx.errorHandler.printError(s_session.group, m_line, fmt::format("Variable {} is defined twice", m_name));
		return PATCH_RESOLVE_RESULT::VARIABLE_CONFLICT;
	}
	return PATCH_RESOLVE_RESULT::RESOLVED;
}

// ---------------------------------------------------------------- groups

void GraphicPack2::ApplyPatchGroups(std::vector<PatchGroup*>& groups, const RPLModule* rpl)
{
	PatchContext_t ctx{};
	ctx.graphicPack = this;
	ctx.matchedModule = rpl;
	ctx.errorHandler.setCurrentGraphicPack(this);
	ctx.errorHandler.setStage(PatchErrorHandler::STAGE::APPLY);
	s_session = ResolveSession{};
	s_session.context = &ctx;

	for (PatchGroup* group : groups)
	{
		if (group->isApplied())
		{
			ctx.errorHandler.printError(group, -1, "This group is already applied to another module");
			return;
		}
	}

	for (PatchGroup* group : groups)
	{
		if (group->m_codeCaveSize == 0)
		{
			cemuLog_log(LogType::Force, "Applying patch group '{}'", group->m_name);
			group->m_codeCave = nullptr;
			continue;
		}
		MEMPTR<void> cave = RPLLoader_AllocateCodeCaveMem(256, group->m_codeCaveSize);
		if (!cave)
		{
			ctx.errorHandler.printError(group, -1, fmt::format("The code cave needs {} bytes but not enough is left (all packs share {} bytes)", group->m_codeCaveSize, MEMORY_CODECAVEAREA_SIZE));
			ctx.errorHandler.showStageErrorMessageBox();
			return;
		}
		cemuLog_log(LogType::Force, "Applying patch group '{}' (code cave {:08x}-{:08x})", group->m_name, cave.GetMPTR(), cave.GetMPTR() + group->m_codeCaveSize);
		group->m_codeCave = cave;
	}

	// Entries may refer to symbols defined later or in other groups, so resolving takes
	// several passes. It stops when a pass makes no progress or after a fixed number of passes.
	std::vector<PendingEntries> pending;
	for (PatchGroup* group : groups)
		pending.push_back({group, group->m_entries});

	constexpr sint32 kMaxPasses = 30;
	for (sint32 pass = 1; pass <= kMaxPasses; pass++)
	{
		const sint32 progress = ResolvePass(ctx, pending, false);
		if (AllResolved(pending))
			break;
		if (progress != 0 && pass < kMaxPasses)
			continue;
		// stuck: one more pass that records what is missing, then report
		ctx.unresolvedSymbols.clear();
		ResolvePass(ctx, pending, true);
		if (pass == kMaxPasses)
			ctx.errorHandler.printError(nullptr, -1, "Some symbols depend on each other too deeply to be resolved");
		for (const auto& missing : ctx.unresolvedSymbols)
			ctx.errorHandler.printError(missing.patchGroup, missing.lineNumber, fmt::format("Unresolved symbol: {}", missing.symbolName));
		ctx.errorHandler.showStageErrorMessageBox();
		return;
	}
	if (!AllResolved(pending) || ctx.errorHandler.hasError())
	{
		ctx.errorHandler.showStageErrorMessageBox();
		return;
	}

	for (PatchGroup* group : groups)
	{
		for (PatchEntry* entry : group->m_entries)
		{
			if (auto* instruction = dynamic_cast<PatchEntryInstruction*>(entry))
				instruction->applyPatch();
		}
		for (const auto& [symbolName, type] : group->m_callbackSymbols)
		{
			auto it = ctx.map_values.find(symbolName);
			if (it == ctx.map_values.end())
			{
				ctx.errorHandler.printError(group, -1, fmt::format(".callback symbol {} is not defined", symbolName));
				ctx.errorHandler.showStageErrorMessageBox();
				return;
			}
			m_callbacks.emplace_back(it->second, type);
		}
	}
	for (PatchGroup* group : groups)
		group->setApplied();
}

void GraphicPack2::UndoPatchGroups(std::vector<PatchGroup*>& groups, const RPLModule* rpl)
{
	for (PatchGroup* group : groups)
	{
		if (!group->isApplied())
			continue;
		for (PatchEntry* entry : group->m_entries)
		{
			if (auto* instruction = dynamic_cast<PatchEntryInstruction*>(entry))
				instruction->undoPatch();
		}
	}
	for (PatchGroup* group : groups)
		group->resetApplied();
}
