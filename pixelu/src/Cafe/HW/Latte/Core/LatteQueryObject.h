// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// host occlusion query, implemented by each backend
class LatteQueryObject
{
public:
	// returns false while the result is not available yet
	virtual bool getResult(uint64& numSamplesPassed) = 0;
	virtual void begin() = 0;
	virtual void end() = 0;

	uint32 index{};
	bool queryEnded{}; // end() was issued; the result can still take a while
	uint64 queryEventStart{};
	uint64 queryEventEnd{};
};
