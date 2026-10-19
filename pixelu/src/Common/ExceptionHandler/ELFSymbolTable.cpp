// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/ExceptionHandler/ELFSymbolTable.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

ELFSymbolTable::ELFSymbolTable()
{
	const int fd = open("/proc/self/exe", O_RDONLY);
	if (fd < 0)
		return;
	struct stat info;
	if (fstat(fd, &info) != 0 || info.st_size <= 0)
	{
		close(fd);
		return;
	}
	void* mapping = mmap(nullptr, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd); // the mapping stays valid
	if (mapping == MAP_FAILED)
		return;
	m_image = (uint8*)mapping;
	m_imageSize = (size_t)info.st_size;

	if (m_imageSize < sizeof(Elf64_Ehdr) || memcmp(m_image, ELFMAG, SELFMAG) != 0)
		return;
	const Elf64_Ehdr* elfHeader = (const Elf64_Ehdr*)m_image;
	if (elfHeader->e_shoff == 0 || elfHeader->e_shoff + (uint64)elfHeader->e_shnum * sizeof(Elf64_Shdr) > m_imageSize)
		return;

	m_sections = std::span<Elf64_Shdr>((Elf64_Shdr*)(m_image + elfHeader->e_shoff), elfHeader->e_shnum);
	if (elfHeader->e_shstrndx < m_sections.size())
		m_sectionNames = SectionAsArray<char>(&m_sections[elfHeader->e_shstrndx]);
	m_symbolNames = SectionAsArray<char>(FindSection(SHT_STRTAB, ".strtab"));
	m_symbols = SectionAsArray<Elf64_Sym>(FindSection(SHT_SYMTAB, ".symtab"));
}

ELFSymbolTable::~ELFSymbolTable()
{
	if (m_image)
		munmap(m_image, m_imageSize);
}

template<typename T>
std::span<T> ELFSymbolTable::SectionAsArray(const Elf64_Shdr* section)
{
	if (!section || section->sh_size < sizeof(T))
		return {};
	if (section->sh_entsize != 0 && section->sh_entsize != sizeof(T))
		return {};
	if (section->sh_offset + section->sh_size > m_imageSize)
		return {};
	return std::span<T>((T*)(m_image + section->sh_offset), section->sh_size / sizeof(T));
}

Elf64_Shdr* ELFSymbolTable::FindSection(Elf64_Word type, const std::string_view& name)
{
	if (m_sectionNames.empty())
		return nullptr;
	for (Elf64_Shdr& section : m_sections)
	{
		if (section.sh_type != type || section.sh_name >= m_sectionNames.size())
			continue;
		if (std::string_view(m_sectionNames.data() + section.sh_name) == name)
			return &section;
	}
	return nullptr;
}

std::string_view ELFSymbolTable::OffsetToSymbol(uint64 ptr, uint64& fromStart) const
{
	fromStart = (uint64)-1;
	if (m_symbols.empty() || m_symbolNames.empty())
		return {};
	// entry 0 is the reserved null symbol
	for (size_t i = 1; i < m_symbols.size(); i++)
	{
		const Elf64_Sym& sym = m_symbols[i];
		if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC)
			continue;
		if (ptr < sym.st_value || ptr - sym.st_value >= sym.st_size)
			continue;
		if (sym.st_name >= m_symbolNames.size())
			return {};
		fromStart = ptr - sym.st_value;
		return std::string_view(m_symbolNames.data() + sym.st_name);
	}
	return {};
}
