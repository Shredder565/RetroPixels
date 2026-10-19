// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

class PatchGroup;

#include "GraphicPackError.h"

// State shared by every entry while the patch groups of one pack are applied to one module.
struct PatchContext_t
{
	// a symbol that could not be resolved, remembered for the error report
	struct UnresolvedSymbol
	{
		sint32 lineNumber;
		PatchGroup* patchGroup;
		std::string symbolName;

		UnresolvedSymbol(sint32 _lineNumber, PatchGroup* _patchGroup, std::string _symbolName)
			: lineNumber(_lineNumber), patchGroup(_patchGroup), symbolName(std::move(_symbolName)) {}

		// report order: by line, then by name
		bool operator<(const UnresolvedSymbol& other) const
		{
			if (lineNumber != other.lineNumber)
				return lineNumber < other.lineNumber;
			return symbolName < other.symbolName;
		}
	};

	class GraphicPack2* graphicPack;
	const RPLModule* matchedModule;
	std::unordered_map<std::string, uint32> map_values; // every symbol, label and variable resolved so far
	std::set<UnresolvedSymbol> unresolvedSymbols;
	PatchErrorHandler errorHandler{};
};

enum class PATCH_RESOLVE_RESULT
{
	RESOLVED,
	EXPRESSION_ERROR, // the expression itself is malformed
	VALUE_ERROR, // the expression has a value but it cannot be used (for example a branch that is too far)
	UNKNOWN_VARIABLE, // depends on something not resolved yet, worth another pass
	VARIABLE_CONFLICT, // the name is already taken
	INVALID_ADDRESS, // the address is neither in the code cave nor in a section of the module
	UNDEFINED_ERROR,
};

enum class EXPRESSION_RESOLVE_RESULT
{
	AVAILABLE,
	EXPRESSION_ERROR,
	UNKNOWN_VARIABLE
};

// one line of a patch file that needs work at apply time
class PatchEntry
{
public:
	PatchEntry() = default;
	virtual ~PatchEntry() = default;

	virtual sint32 getLineNumber() { return -1; }
	virtual PATCH_RESOLVE_RESULT resolve(PatchContext_t& ctx) = 0;
};

// Cemuhook style "<name> = <expression>": the value is an address and gets relocated
class PatchEntryCemuhookSymbolValue : public PatchEntry
{
public:
	PatchEntryCemuhookSymbolValue(sint32 lineNumber, const char* symbolName, const sint32 symbolNameLen, const char* expressionStr, const sint32 expressionLen)
		: m_line(lineNumber), m_name(symbolName, symbolNameLen), m_expression(expressionStr, expressionLen) {}

	sint32 getLineNumber() override { return m_line; }
	PATCH_RESOLVE_RESULT resolve(PatchContext_t& ctx) override;
	std::string& getSymbolName() { return m_name; }

private:
	sint32 m_line;
	std::string m_name;
	std::string m_expression;
	uint32 m_address{};
};

enum class PATCHVARTYPE
{
	DOUBLE,
	INT, // signed 32 bit
	UINT, // unsigned 32 bit, also used for pointers
};

// Cemu style "<name> = <expression>": a plain value, never relocated
class PatchEntryVariableValue : public PatchEntry
{
public:
	PatchEntryVariableValue(sint32 lineNumber, const char* symbolName, const sint32 symbolNameLen, PATCHVARTYPE varType, const char* expressionStr, const sint32 expressionLen)
		: m_line(lineNumber), m_name(symbolName, symbolNameLen), m_expression(expressionStr, expressionLen), m_type(varType) {}

	sint32 getLineNumber() override { return m_line; }
	PATCH_RESOLVE_RESULT resolve(PatchContext_t& ctx) override;
	std::string& getSymbolName() { return m_name; }

private:
	sint32 m_line;
	std::string m_name;
	std::string m_expression;
	PATCHVARTYPE m_type;
};

// "<name>:" placed at an address of the patch
class PatchEntryLabel : public PatchEntry
{
public:
	PatchEntryLabel(sint32 lineNumber, const char* symbolName, const sint32 symbolNameLen)
		: m_line(lineNumber), m_name(symbolName, symbolNameLen) {}

	sint32 getLineNumber() override { return m_line; }
	PATCH_RESOLVE_RESULT resolve(PatchContext_t& ctx) override;
	std::string& getSymbolName() { return m_name; }
	uint32 getSymbolValue() { return m_finalAddress; }
	void setAssignedVA(uint32 virtualAddress) { m_patchAddress = virtualAddress; }

private:
	sint32 m_line;
	std::string m_name;
	uint32 m_patchAddress{};
	uint32 m_finalAddress{};
};

// bytes produced by the assembler for one line, written to memory when the group is applied
class PatchEntryInstruction : public PatchEntry
{
public:
	PatchEntryInstruction(sint32 lineNumber, uint32 patchAddr, std::span<uint8> data, std::vector<PPCAssemblerReloc>& list_relocs)
		: m_line(lineNumber), m_patchAddress(patchAddr), m_assembled(data.begin(), data.end()), m_final(data.begin(), data.end()), m_saved(data.size()), m_relocs(list_relocs) {}

	sint32 getLineNumber() override { return m_line; }
	uint32 getAddr() const { return m_patchAddress; }
	uint32 getRelocatedAddr() { return m_finalAddress; }
	uint32 getSize() const { return (uint32)m_assembled.size(); }

	PATCH_RESOLVE_RESULT resolve(PatchContext_t& ctx) override;
	PATCH_RESOLVE_RESULT resolveReloc(PatchContext_t& ctx, PPCAssemblerReloc* reloc);

	void applyPatch();
	void undoPatch();

private:
	sint32 m_line;
	uint32 m_patchAddress; // as written in the patch file
	uint32 m_finalAddress{}; // after relocation
	bool m_hasFinalAddress{};
	std::vector<uint8> m_assembled; // assembler output
	std::vector<uint8> m_final; // assembler output with relocations filled in
	std::vector<uint8> m_saved; // memory contents before the patch was written
	std::vector<PPCAssemblerReloc> m_relocs;
};

enum class GPCallbackType {
	Entry
};

// one [group] of a patch file
class PatchGroup
{
	friend class GraphicPack2;

public:
	// addresses below this are offsets into the group's code cave
	static constexpr uint32 CODECAVE_MAX_OFFSET = 8 * 1024 * 1024;

	PatchGroup(class GraphicPack2* gp, const char* nameStr, sint32 nameLength) : m_pack(gp), m_name(nameStr, nameLength) {}
	~PatchGroup()
	{
		for (PatchEntry* e : m_entries)
			delete e;
	}

	bool matchesCRC(uint32 crc)
	{
		return std::find(m_moduleChecksums.begin(), m_moduleChecksums.end(), crc) != m_moduleChecksums.end();
	}

	uint32 getCodeCaveBase() { return m_codeCave.GetMPTR(); }
	uint32 getCodeCaveSize() { return m_codeCaveSize; }
	std::string_view getName() { return m_name; }

	void setApplied() { m_applied = true; }
	void resetApplied() { m_applied = false; }
	bool isApplied() const { return m_applied; }

private:
	class GraphicPack2* m_pack;
	std::string m_name;
	std::vector<uint32> m_moduleChecksums;
	bool m_targetsRpx{}; // "moduleMatches = rpx"
	std::vector<PatchEntry*> m_entries; // owned
	std::vector<std::pair<std::string, GPCallbackType>> m_callbackSymbols;
	uint32 m_codeCaveSize{};
	MEMPTR<void> m_codeCave;
	bool m_applied{};
};
