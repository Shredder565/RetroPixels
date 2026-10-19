// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"

namespace
{
	struct SymbolTable
	{
		std::mutex lock;
		std::unordered_map<uint32, RPLStoredSymbol*> byAddress;
		std::deque<std::string> strings; // deque keeps element addresses stable
		std::vector<const char*> libNames; // interned library names, matched case-insensitively
	};

	SymbolTable& Table()
	{
		static SymbolTable s_table;
		return s_table;
	}

	const char* KeepString(SymbolTable& t, const char* s)
	{
		return t.strings.emplace_back(s).c_str();
	}

	const char* KeepLibName(SymbolTable& t, const char* lib)
	{
		for (const char* known : t.libNames)
		{
			if (boost::iequals(known, lib))
				return known;
		}
		const char* stored = KeepString(t, lib);
		t.libNames.push_back(stored);
		return stored;
	}

	void FreeChain(RPLStoredSymbol* s)
	{
		while (s)
		{
			RPLStoredSymbol* older = s->previous;
			delete s;
			s = older;
		}
	}
}

RPLStoredSymbol* rplSymbolStorage_store(const char* libName, const char* symbolName, MPTR address, uint32 type)
{
	SymbolTable& t = Table();
	std::lock_guard _l(t.lock);
	auto* sym = new RPLStoredSymbol();
	sym->address = address;
	sym->libName = (void*)KeepLibName(t, libName);
	sym->symbolName = (void*)KeepString(t, symbolName);
	sym->flags = type;
	RPLStoredSymbol*& head = t.byAddress[address];
	sym->previous = head; // null for a new address
	head = sym;
	return sym;
}

RPLStoredSymbol* rplSymbolStorage_getByAddress(MPTR address)
{
	SymbolTable& t = Table();
	std::lock_guard _l(t.lock);
	auto it = t.byAddress.find(address);
	return it != t.byAddress.end() ? it->second : nullptr;
}

RPLStoredSymbol* rplSymbolStorage_getByClosestAddress(MPTR address)
{
	SymbolTable& t = Table();
	std::lock_guard _l(t.lock);
	for (uint32 step = 0; step < 4096; step++, address -= 4)
	{
		auto it = t.byAddress.find(address);
		if (it != t.byAddress.end())
			return it->second;
	}
	return nullptr;
}

void rplSymbolStorage_remove(RPLStoredSymbol* storedSymbol)
{
	SymbolTable& t = Table();
	std::lock_guard _l(t.lock);
	auto it = t.byAddress.find(storedSymbol->address);
	if (it != t.byAddress.end())
	{
		// unlink from the chain of its address
		RPLStoredSymbol** link = &it->second;
		while (*link && *link != storedSymbol)
			link = &(*link)->previous;
		if (*link)
			*link = storedSymbol->previous;
		if (!it->second)
			t.byAddress.erase(it);
	}
	delete storedSymbol;
}

void rplSymbolStorage_removeRange(MPTR address, sint32 length, uint32 type)
{
	SymbolTable& t = Table();
	std::lock_guard _l(t.lock);
	for (; length > 0; address += 4, length -= 4)
	{
		auto it = t.byAddress.find(address);
		if (it == t.byAddress.end())
			continue;
		RPLStoredSymbol** link = &it->second;
		while (*link)
		{
			RPLStoredSymbol* s = *link;
			if ((s->flags & type) == type)
			{
				*link = s->previous;
				delete s;
			}
			else
				link = &s->previous;
		}
		if (!it->second)
			t.byAddress.erase(it);
	}
}

void rplSymbolStorage_createJumpProxySymbol(MPTR jumpAddress, MPTR destAddress)
{
	RPLStoredSymbol* dest = rplSymbolStorage_getByAddress(destAddress);
	if (dest)
		rplSymbolStorage_store((const char*)dest->libName, (const char*)dest->symbolName, jumpAddress);
}

std::unordered_map<uint32, RPLStoredSymbol*>& rplSymbolStorage_lockSymbolMap()
{
	SymbolTable& t = Table();
	t.lock.lock();
	return t.byAddress;
}

void rplSymbolStorage_unlockSymbolMap()
{
	Table().lock.unlock();
}

void rplSymbolStorage_init()
{
	cemu_assert_debug(Table().byAddress.empty());
}

void rplSymbolStorage_unloadAll()
{
	SymbolTable& t = Table();
	std::lock_guard _l(t.lock);
	for (auto& it : t.byAddress)
		FreeChain(it.second);
	t.byAddress.clear();
	t.libNames.clear();
	t.strings.clear();
}
