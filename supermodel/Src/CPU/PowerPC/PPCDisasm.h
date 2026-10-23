// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// PPCDisasm.h: turns one PowerPC 603e instruction word into text.

#ifndef INCLUDED_PPCDISASM_H
#define INCLUDED_PPCDISASM_H

#include "Types.h"

// Writes the mnemonic of instruction word `op` (found at address `vpc`) to
// `mnem` and its operands to `oprs` (at most `oprs_size` bytes). With
// `simplify`, common idioms are shown in their shorter forms (li, mr, nop,
// bt/bf, ...).
//
// Returns OKAY for a valid instruction. FAIL means either that the word is
// not an instruction (mnem and oprs are left empty) or that it is an
// instruction in an invalid form, such as an update load whose base register
// is also its target (mnem and oprs are still filled in).
//
// `mnem` must have room for at least 16 characters.
extern Result DisassemblePowerPC(UINT32 op, UINT32 vpc, char *mnem, char *oprs, size_t oprs_size,
                                 bool simplify);

#endif // INCLUDED_PPCDISASM_H
