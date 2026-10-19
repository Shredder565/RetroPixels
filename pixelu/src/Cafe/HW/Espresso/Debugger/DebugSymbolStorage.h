// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/helpers/fspinlock.h"

// What kind of value lives at a guest address. Graphic pack patches record
// this so the disassembler can show data words in a readable form.
enum class DEBUG_SYMBOL_TYPE
{
	UNDEFINED,
	CODE,
	// data, stored big-endian in guest memory
	U64,
	U32,
	U16,
	U8,
	S64,
	S32,
	S16,
	S8,
	FLOAT,
	DOUBLE,
};

class DebugSymbolStorage
{
public:
	static void StoreDataType(MPTR address, DEBUG_SYMBOL_TYPE type)
	{
		std::lock_guard guard(s_lock);
		s_typeStorage.insert_or_assign(address, type);
	}

	static DEBUG_SYMBOL_TYPE GetDataType(MPTR address)
	{
		std::lock_guard guard(s_lock);
		auto entry = s_typeStorage.find(address);
		return entry != s_typeStorage.end() ? entry->second : DEBUG_SYMBOL_TYPE::UNDEFINED;
	}

	// forgets the annotations of every word slot in [address, address + length)
	static void ClearRange(MPTR address, uint32 length)
	{
		std::lock_guard guard(s_lock);
		for (uint32 offset = 0; offset < length; offset += 4)
		{
			s_typeStorage.erase((MPTR)(address + offset));
			if (length - offset <= 4)
				break;
		}
	}

private:
	static FSpinlock s_lock;
	static std::unordered_map<MPTR, DEBUG_SYMBOL_TYPE> s_typeStorage;
};
