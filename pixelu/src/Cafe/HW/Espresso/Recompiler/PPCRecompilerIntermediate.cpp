// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"

// Code entered from the jump table starts with all guest registers in
// memory. Giving each entry point a block of its own, which nothing else
// branches to, gives the register allocator a place to load the registers
// the entry needs without affecting the other paths into the block.
void PPCRecompilerIML_IsolateEntries(IMLFunction& func)
{
	func.RebuildPredecessors();
	size_t blockCount = func.blocks.size();
	for (size_t i = 0; i < blockCount; i++)
	{
		IMLSegment* target = func.blocks[i];
		if (!target->isEntry || target->preds.empty())
			continue;
		IMLSegment* entry = func.NewBlock();
		entry->isEntry = true;
		entry->entryAddress = target->entryAddress;
		entry->ppcAddress = target->ppcAddress;
		entry->ppcEndAddress = target->ppcAddress;
		entry->Append(IMLInstruction::Make(IMLOp::JUMP));
		entry->taken = target;
		target->isEntry = false;
		target->entryAddress = 0;
	}
	func.Renumber();
	func.RebuildPredecessors();
}
