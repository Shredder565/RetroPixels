// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Interface of a Cafe OS library implemented in C++ instead of being loaded
// from an RPL file.
#pragma once

namespace coreinit
{
	enum class RplEntryReason;
};

class COSModule
{
public:
	virtual std::string_view GetName() = 0;

	// other modules that get loaded along with this one
	virtual std::vector<std::string_view> GetDependencies() { return {}; };

	virtual void RPLMapped() {}; // the module became part of the process
	virtual void RPLUnmapped() {}; // the module left the process

	// counterpart of the guest module entry point. Unload notifications are also
	// sent when the emulated process is torn down, together with RPLUnmapped()
	virtual void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) {};
};

std::span<COSModule*> GetCOSModules();
