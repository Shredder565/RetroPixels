// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Common/FileStream.h"
#include "util/helpers/StringParser.h"
#include "Cemu/PPCAssembler/ppcAssembler.h"
#include "Cafe/OS/RPL/rpl_structs.h"

namespace
{
	constexpr uint32 kNoAddress = 0xFFFFFFFF;
	// origins past this point no longer grow the code cave
	constexpr uint32 kCodeCaveLimit = 32 * 1024 * 1024;

	bool StartsWithHexPrefix(StringTokenParser& line)
	{
		return line.compareCharacter(0, '0') && line.compareCharacterI(1, 'x');
	}

	// "[name]" with nothing after it; on success name points into the line
	enum class GroupHeader
	{
		NotAHeader,
		Ok,
		MissingBracket,
		TrailingText,
	};

	GroupHeader ReadGroupHeader(StringTokenParser& line, const char*& name, sint32& nameLength)
	{
		if (!line.compareCharacter(0, '['))
			return GroupHeader::NotAHeader;
		line.skipCharacters(1);
		name = line.getCurrentPtr();
		nameLength = line.skipToCharacter(']');
		if (nameLength < 0)
			return GroupHeader::MissingBracket;
		line.skipCharacters(1);
		line.skipWhitespaces();
		return line.isEndOfString() ? GroupHeader::Ok : GroupHeader::TrailingText;
	}

	// the rest of the line with the white space on both ends removed
	std::string_view RestOfLine(StringTokenParser& line)
	{
		line.skipWhitespaces();
		line.trimWhitespaces();
		return std::string_view(line.getCurrentPtr(), std::max<sint32>(line.getCurrentLen(), 0));
	}

	PatchEntryInstruction* MakeInstruction(sint32 lineNumber, uint32 address, PPCAssemblerInOut& assembled)
	{
		return new PatchEntryInstruction(lineNumber, address, {assembled.outputData.data(), assembled.outputData.size()}, assembled.list_relocs);
	}
}

// length of the line without a trailing # or ; comment (quoted text may contain both)
sint32 GraphicPack2::GetLengthWithoutComment(const char* str, size_t length)
{
	bool quoted = false;
	for (size_t i = 0; i < length; i++)
	{
		const char c = str[i];
		if (c == '"')
			quoted = !quoted;
		else if (!quoted && (c == '#' || c == ';'))
			return (sint32)i;
	}
	return (sint32)length;
}

// logs the problem and drops every group loaded so far
void GraphicPack2::LogPatchesSyntaxError(sint32 lineNumber, std::string_view errorMsg)
{
	cemuLog_log(LogType::Force, "Graphic pack {}: syntax error in patches", _pathToUtf8(GetRulesPath()));
	if (lineNumber >= 0)
		cemuLog_log(LogType::Force, "Line {}: {}", lineNumber, errorMsg);
	else
		cemuLog_log(LogType::Force, "{}", errorMsg);
	DropPatchGroups();
}

void GraphicPack2::AddPatchGroup(PatchGroup* group)
{
	if (group->m_moduleChecksums.empty() && !group->m_targetsRpx)
	{
		LogPatchesSyntaxError(-1, fmt::format("Group \"{}\" does not say which modules it is for (moduleMatches)", group->m_name));
		delete group;
		return;
	}
	// the code cave has to cover every instruction placed at a cave offset
	uint32 caveEnd = 0;
	for (PatchEntry* entry : group->m_entries)
	{
		auto* instruction = dynamic_cast<PatchEntryInstruction*>(entry);
		if (instruction && instruction->getAddr() < PatchGroup::CODECAVE_MAX_OFFSET)
			caveEnd = std::max(caveEnd, instruction->getAddr() + instruction->getSize());
	}
	if (group->m_entries.size() < (caveEnd / 4) / 8)
		cemuLog_log(LogType::Force, "Graphic pack {}: the code cave of group [{}] reaches 0x{:x} but holds few entries", m_name, group->m_name, caveEnd);
	group->m_codeCaveSize = caveEnd;
	m_patchGroups.push_back(group);
}

// patches.txt (Cemuhook format): every patched instruction has an explicit address
void GraphicPack2::ParseCemuhookPatchesTxtInternal(MemStreamReader& patchesStream)
{
	PatchGroup* group = nullptr;
	auto fail = [&](sint32 lineNumber, std::string_view message) {
		LogPatchesSyntaxError(lineNumber, message);
		delete group;
	};

	for (sint32 lineNumber = 1;; lineNumber++)
	{
		std::string_view text = patchesStream.readLine();
		if (patchesStream.hasError())
			break;
		StringTokenParser line(text.data(), GetLengthWithoutComment(text.data(), text.size()));
		line.skipWhitespaces();
		if (line.isEndOfString())
			continue;

		const char* name;
		sint32 nameLength;
		switch (ReadGroupHeader(line, name, nameLength))
		{
		case GroupHeader::Ok:
			if (group)
				AddPatchGroup(group);
			group = new PatchGroup(this, name, nameLength);
			continue;
		case GroupHeader::MissingBracket:
			return fail(lineNumber, "Group name is missing the closing ']'");
		case GroupHeader::TrailingText:
			return fail(lineNumber, "Unexpected text after the group name");
		case GroupHeader::NotAHeader:
			break;
		}

		if (StartsWithHexPrefix(line))
		{
			// <address> = <instruction>
			uint32 address;
			if (!line.parseU32(address))
				return fail(lineNumber, "Address is not a valid number");
			if (!line.matchWordI("="))
				return fail(lineNumber, "Expected '=' after the address");
			if (!group)
				return fail(lineNumber, "Instruction outside of a group");
			PPCAssemblerInOut assembled{};
			assembled.virtualAddress = address;
			const std::string instruction(RestOfLine(line));
			if (!ppcAssembler_assembleSingleInstruction(instruction.c_str(), &assembled))
				return fail(lineNumber, fmt::format("Assembler: {}", assembled.errorMsg));
			group->m_entries.push_back(MakeInstruction(lineNumber, address, assembled));
			continue;
		}

		if (line.matchWordI("moduleMatches"))
		{
			if (!group)
				return fail(lineNumber, "moduleMatches outside of a group");
			if (!line.matchWordI("="))
				return fail(lineNumber, "Expected '=' after moduleMatches");
			do
			{
				uint32 checksum = 0;
				if (!line.parseU32(checksum))
					return fail(lineNumber, "moduleMatches has an invalid checksum");
				group->m_moduleChecksums.push_back(checksum);
			} while (line.matchWordI(","));
			line.skipWhitespaces();
			if (!line.isEndOfString())
				return fail(lineNumber, "Unexpected text after moduleMatches");
			continue;
		}

		// <symbol> = <expression>; symbols may start with a letter as well as with '_'
		const char* symbol;
		sint32 symbolLength;
		if (line.parseSymbolName(symbol, symbolLength) && line.matchWordI("="))
		{
			if (!group)
				return fail(lineNumber, "Symbol outside of a group");
			std::string_view expression = RestOfLine(line);
			group->m_entries.push_back(new PatchEntryCemuhookSymbolValue(lineNumber, symbol, symbolLength, expression.data(), (sint32)expression.size()));
			continue;
		}
		return fail(lineNumber, "Line not understood");
	}
	if (group)
		AddPatchGroup(group);
}

// patch_*.asm (Cemu format): code is placed at a running origin, labels and variables can be used
bool GraphicPack2::ParseCemuPatchesTxtInternal(MemStreamReader& patchesStream)
{
	PatchGroup* group = nullptr;
	// where the next instruction goes; starts out usable (0) until the first group header resets it
	uint32 origin = 0;
	// highest origin reached in the current group, ".origin = codecave" continues from there
	uint32 caveHighWater = 0;
	// labels on their own line wait for the next instruction so they end up after its alignment padding
	std::vector<PatchEntryLabel*> pendingLabels;

	auto advance = [&](uint32 bytes) {
		origin += bytes;
		if (origin <= kCodeCaveLimit)
			caveHighWater = std::max(caveHighWater, origin);
	};
	auto placePendingLabels = [&](uint32 address) {
		for (PatchEntryLabel* label : pendingLabels)
		{
			label->setAssignedVA(address);
			group->m_entries.push_back(label);
		}
		pendingLabels.clear();
	};
	auto fail = [&](sint32 lineNumber, std::string_view message) {
		LogPatchesSyntaxError(lineNumber, message);
		for (PatchEntryLabel* label : pendingLabels)
			delete label;
		delete group;
		return false;
	};

	for (sint32 lineNumber = 1;; lineNumber++)
	{
		std::string_view text = patchesStream.readLine();
		if (patchesStream.hasError())
			break;
		StringTokenParser line(text.data(), GetLengthWithoutComment(text.data(), text.size()));
		line.skipWhitespaces();
		if (line.isEndOfString())
			continue;

		const char* name;
		sint32 nameLength;
		switch (ReadGroupHeader(line, name, nameLength))
		{
		case GroupHeader::Ok:
			if (group)
			{
				placePendingLabels(origin);
				AddPatchGroup(group);
			}
			group = new PatchGroup(this, name, nameLength);
			origin = kNoAddress;
			caveHighWater = 0;
			continue;
		case GroupHeader::MissingBracket:
			return fail(lineNumber, "Group name is missing the closing ']'");
		case GroupHeader::TrailingText:
			return fail(lineNumber, "Unexpected text after the group name");
		case GroupHeader::NotAHeader:
			break;
		}

		if (line.matchWordI("moduleMatches"))
		{
			if (!group)
				return fail(lineNumber, "moduleMatches outside of a group");
			if (!line.matchWordI("="))
				return fail(lineNumber, "Expected '=' after moduleMatches");
			while (true)
			{
				if (line.matchWordI("rpx"))
				{
					group->m_targetsRpx = true;
					break;
				}
				uint32 checksum = 0;
				if (!line.parseU32(checksum))
					return fail(lineNumber, "moduleMatches has an invalid checksum");
				group->m_moduleChecksums.push_back(checksum);
				if (!line.matchWordI(","))
					break;
			}
			line.skipWhitespaces();
			if (!line.isEndOfString())
				return fail(lineNumber, "Unexpected text after moduleMatches");
			continue;
		}

		// "<address> = ..." places just this line at a fixed address
		uint32 fixedAddress = kNoAddress;
		if (StartsWithHexPrefix(line))
		{
			if (!line.parseU32(fixedAddress))
				return fail(lineNumber, "Address is not a valid number");
			if (!line.matchWordI("="))
				return fail(lineNumber, "Expected '=' after the address");
			line.skipWhitespaces();
			line.trimWhitespaces();
		}

		if (line.matchWordI(".origin"))
		{
			if (fixedAddress != kNoAddress)
				return fail(lineNumber, ".origin cannot have an address prefix");
			if (!line.matchWordI("="))
				return fail(lineNumber, "Expected '=' after .origin");
			uint32 newOrigin;
			if (line.matchWordI("codecave"))
				origin = caveHighWater;
			else if (line.parseU32(newOrigin))
				origin = newOrigin;
			else
				return fail(lineNumber, ".origin must be set to an address or to codecave");
			continue;
		}

		if (line.matchWordI(".callback"))
		{
			if (!line.matchWordI("entry"))
				return fail(lineNumber, ".callback type not supported");
			const char* symbol;
			sint32 symbolLength;
			if (!line.parseSymbolName(symbol, symbolLength))
				return fail(lineNumber, ".callback needs a symbol after the type");
			if (!group)
				return fail(lineNumber, ".callback outside of a group");
			group->m_callbackSymbols.emplace_back(std::string(symbol, (size_t)symbolLength), GPCallbackType::Entry);
			continue;
		}

		StringTokenParser lineStart;
		line.storeParserState(&lineStart);
		const char* symbol;
		sint32 symbolLength;

		// <label>:
		if (line.parseSymbolName(symbol, symbolLength) && line.matchWordI(":"))
		{
			line.skipWhitespaces();
			if (!line.isEndOfString())
				return fail(lineNumber, "Unexpected text after the label");
			if (fixedAddress == kNoAddress)
			{
				if (origin == kNoAddress)
					return fail(lineNumber, "Label without an address (set .origin or use <address> = )");
				pendingLabels.push_back(new PatchEntryLabel(lineNumber, symbol, symbolLength));
				continue;
			}
			if (!group)
				return fail(lineNumber, "Label outside of a group");
			auto* label = new PatchEntryLabel(lineNumber, symbol, symbolLength);
			label->setAssignedVA(fixedAddress);
			group->m_entries.push_back(label);
			continue;
		}

		// <variable> = <expression>
		line.restoreParserState(&lineStart);
		if (line.parseSymbolName(symbol, symbolLength) && line.matchWordI("="))
		{
			if (!group)
				return fail(lineNumber, "Variable outside of a group");
			std::string_view expression = RestOfLine(line);
			group->m_entries.push_back(new PatchEntryVariableValue(lineNumber, symbol, symbolLength, PATCHVARTYPE::UINT, expression.data(), (sint32)expression.size()));
			continue;
		}

		// anything else is handed to the assembler
		line.restoreParserState(&lineStart);
		PPCAssemblerInOut assembled{};
		if (fixedAddress != kNoAddress)
		{
			assembled.virtualAddress = fixedAddress;
			assembled.forceNoAlignment = true; // a fixed address is used exactly as given
		}
		else if (origin != kNoAddress)
			assembled.virtualAddress = origin;
		else
			return fail(lineNumber, "Instruction without an address (set .origin or use <address> = )");
		if (!group)
			return fail(lineNumber, "Instruction outside of a group");
		const std::string instruction(line.getCurrentPtr(), std::max<sint32>(line.getCurrentLen(), 0));
		if (!ppcAssembler_assembleSingleInstruction(instruction.c_str(), &assembled))
			return fail(lineNumber, fmt::format("Assembler: {}", assembled.errorMsg));
		cemu_assert_debug(assembled.alignmentRequirement != 0);
		if (fixedAddress == kNoAddress)
		{
			advance(assembled.alignmentPaddingSize);
			advance((uint32)assembled.outputData.size());
		}
		placePendingLabels(assembled.virtualAddressAligned);
		group->m_entries.push_back(MakeInstruction(lineNumber, assembled.virtualAddressAligned, assembled));
	}

	if (group)
	{
		placePendingLabels(origin);
		AddPatchGroup(group);
	}
	else
	{
		for (PatchEntryLabel* label : pendingLabels)
			delete label;
	}
	return true;
}
