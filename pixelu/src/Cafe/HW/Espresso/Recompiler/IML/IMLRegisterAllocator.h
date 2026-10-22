// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "IML.h"

// Register allocation.
//
// Guest registers are kept in host registers while the function works with
// them; their home slots in PPCInterpreter_t are updated right after every
// write (stores that a later write in the same block makes redundant are
// left out). This keeps the home slots current wherever the function can be
// left, so exits and interpreter calls need no extra stores.
//
// Each virtual register gets one host register (a "color") for the whole
// function, chosen with a priority based graph coloring. Registers that do
// not get a color are spilled: they live in their home slot (guest
// registers) or in a stack slot (temporaries) and every access goes through
// a short lived temporary.

struct IMLRegisterAllocatorParameters
{
	uint32 intRegCount{0};          // usable integer host registers (colors 0..n-1)
	uint32 floatRegCount{0};        // usable vector host registers
	uint32 intCallerSavedMask{0};   // colors a helper call clobbers
	uint32 floatCallerSavedMask{0};
};

// saveMask layout on helper calls: integer colors in bits 0-15, float colors in bits 16-31
constexpr uint32 IML_SAVEMASK_FLOAT_SHIFT = 16;

// adds the guest register loads at entry points and after interpreter calls
// and the home slot stores after every write
void IMLRegisterAllocator_PrepareGuestRegisters(IMLFunction& func);

// assigns host registers (IMLVRegInfo::hostReg) and fills IMLInstruction::saveMask.
// Returns false if no allocation was found
bool IMLRegisterAllocator_AllocateRegisters(IMLFunction& func, const IMLRegisterAllocatorParameters& params);
