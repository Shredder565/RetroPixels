// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/RPL/rpl_debug_symbols.h"

static std::map<MPTR, rplDebugSymbolBase*> s_annotations;

void rplDebugSymbol_createComment(MPTR address, const wchar_t* comment)
{
	auto* entry = new rplDebugSymbolComment();
	entry->type = RplDebugSymbolComment;
	entry->next = nullptr;
	entry->comment = comment;
	rplDebugSymbolBase*& slot = s_annotations[address];
	delete static_cast<rplDebugSymbolComment*>(slot); // only comments exist
	slot = entry;
}

rplDebugSymbolBase* rplDebugSymbol_getForAddress(MPTR address)
{
	auto it = s_annotations.find(address);
	return it != s_annotations.end() ? it->second : nullptr;
}

const std::map<MPTR, rplDebugSymbolBase*>& rplDebugSymbol_getSymbols()
{
	return s_annotations;
}
