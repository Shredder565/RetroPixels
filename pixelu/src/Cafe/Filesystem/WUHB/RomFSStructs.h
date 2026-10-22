// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// On-disk structures of the RomFS image inside a .wuhb (Wii U homebrew
// bundle). All values are big endian. Entries are addressed by their byte
// offset within the directory or file table.

struct romfs_header_t
{
	uint32 header_magic; // "WUHB"
	uint32be header_size;
	uint64be dir_hash_table_ofs;
	uint64be dir_hash_table_size;
	uint64be dir_table_ofs;
	uint64be dir_table_size;
	uint64be file_hash_table_ofs;
	uint64be file_hash_table_size;
	uint64be file_table_ofs;
	uint64be file_table_size;
	uint64be file_partition_ofs; // file data offsets are relative to this
};
static_assert(sizeof(romfs_header_t) == 0x50);

// the fixed part is stored on disk, followed by name_size bytes of name
struct romfs_direntry_t
{
	uint32be parent;
	uint32be listNext;     // next sibling folder
	uint32be dirListHead;  // first child folder
	uint32be fileListHead; // first file
	uint32be hash;         // next entry in the same hash bucket
	uint32be name_size;
	std::string name;
};

struct romfs_fentry_t
{
	uint32be parent;
	uint32be listNext; // next file in the same folder
	uint64be offset;
	uint64be size;
	uint32be hash;     // next entry in the same hash bucket
	uint32be name_size;
	std::string name;
};

#define ROMFS_ENTRY_EMPTY 0xFFFFFFFF
