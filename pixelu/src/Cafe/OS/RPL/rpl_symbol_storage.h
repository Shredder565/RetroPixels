// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Address -> name table used by the debugger and crash logs. Several symbols
// can share an address; the most recently stored one is returned first and
// links to the older ones through 'previous'. Name pointers stay valid until
// rplSymbolStorage_unloadAll().
#pragma once

enum RPLStoredSymbolType : uint32
{
	RPL_STORED_SYMBOL_NONE = 0,
	RPL_STORED_SYMBOL_MAP = 1 << 0, // from a user supplied symbol map
	RPL_STORED_SYMBOL_PATCH = 1 << 1, // from a graphic pack patch
};

struct RPLStoredSymbol
{
	MPTR address;
	void* libName; // const char*
	void* symbolName; // const char*
	uint32 flags;
	RPLStoredSymbol* previous;
};

void rplSymbolStorage_init();
void rplSymbolStorage_unloadAll();
RPLStoredSymbol* rplSymbolStorage_store(const char* libName, const char* symbolName, MPTR address, uint32 type = RPL_STORED_SYMBOL_NONE);
void rplSymbolStorage_remove(RPLStoredSymbol* storedSymbol);
void rplSymbolStorage_removeRange(MPTR address, sint32 length, uint32 type = RPL_STORED_SYMBOL_NONE); // drops symbols whose flags contain all bits of type
RPLStoredSymbol* rplSymbolStorage_getByAddress(MPTR address);
RPLStoredSymbol* rplSymbolStorage_getByClosestAddress(MPTR address); // searches downwards up to 16KB
void rplSymbolStorage_createJumpProxySymbol(MPTR jumpAddress, MPTR destAddress); // names a trampoline after its destination

std::unordered_map<uint32, RPLStoredSymbol*>& rplSymbolStorage_lockSymbolMap();
void rplSymbolStorage_unlockSymbolMap();
