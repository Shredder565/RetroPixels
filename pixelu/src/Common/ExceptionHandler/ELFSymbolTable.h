// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <memory>
#include <elf.h>

// Read-only view of the running executable's static symbol table (.symtab),
// used to name functions in crash backtraces that the dynamic symbol table
// does not cover. The executable is memory-mapped for the object's lifetime.
class ELFSymbolTable
{
public:
	ELFSymbolTable();
	~ELFSymbolTable();

	ELFSymbolTable(const ELFSymbolTable&) = delete;
	ELFSymbolTable& operator=(const ELFSymbolTable&) = delete;

	// Name of the function containing the given file-relative address, and
	// the offset into it. Empty with fromStart = (uint64)-1 if not found.
	std::string_view OffsetToSymbol(uint64 ptr, uint64& fromStart) const;

private:
	Elf64_Shdr* FindSection(Elf64_Word type, const std::string_view& name);

	// a section's contents as an array of T, empty if the section does not fit T
	template<typename T>
	std::span<T> SectionAsArray(const Elf64_Shdr* section);

	uint8* m_image = nullptr;
	size_t m_imageSize = 0;

	std::span<Elf64_Shdr> m_sections{};
	std::span<char> m_sectionNames{};
	std::span<Elf64_Sym> m_symbols{};
	std::span<char> m_symbolNames{};
};
