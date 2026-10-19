// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Annotations the debugger attaches to guest addresses (currently comments).
#pragma once
#include <map>

enum RplDebugSymbolType : uint8
{
	RplDebugSymbolComment = 0,
};

struct rplDebugSymbolBase
{
	RplDebugSymbolType type;
	rplDebugSymbolBase* next;
};

struct rplDebugSymbolComment : rplDebugSymbolBase
{
	std::wstring comment;
};

void rplDebugSymbol_createComment(MPTR address, const wchar_t* comment); // replaces an existing annotation
rplDebugSymbolBase* rplDebugSymbol_getForAddress(MPTR address); // null if none
const std::map<MPTR, rplDebugSymbolBase*>& rplDebugSymbol_getSymbols();
