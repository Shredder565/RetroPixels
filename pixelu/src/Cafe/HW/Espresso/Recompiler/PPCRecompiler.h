// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <boost/container/small_vector.hpp>

// PowerPC to host recompiler.
//
// Guest code is compiled function by function on a background thread. A
// jump table holds one host code pointer per guest instruction address in
// the code area; addresses without compiled code point at one of two exit
// stubs that return to the interpreter (the stub tells whether compilation
// of the address was already requested).

#define PPC_REC_CODE_AREA_START		(0x00000000) // the jump table starts at guest address 0
#define PPC_REC_CODE_AREA_END		(0x10000000)
#define PPC_REC_CODE_AREA_SIZE		(PPC_REC_CODE_AREA_END - PPC_REC_CODE_AREA_START)

#define PPC_REC_ALIGN_TO_4MB(__v)	(((__v)+4*1024*1024-1)&~(4*1024*1024-1))

// a compiled guest function
struct PPCRecCompiledFunction
{
	struct Range
	{
		uint32 start;
		uint32 end;
		void* storeHandle; // handle in the function range store
	};

	uint32 entryAddress;   // address compilation started at
	void* hostCode;
	size_t hostCodeSize;
	std::vector<Range> ranges; // guest code covered by this function
	// jump table entries pointing into this function
	boost::container::small_vector<std::pair<uint32, void*>, 4> entryPoints;
};

typedef void ATTR_MS_ABI (*PPCREC_JUMP_ENTRY)();

struct PPCRecompilerInstanceData_t
{
	// host entry point for every guest instruction in the code area
	PPCREC_JUMP_ENTRY ppcRecompilerDirectJumpTable[PPC_REC_ALIGN_TO_4MB(PPC_REC_CODE_AREA_SIZE / 4)];
};

extern PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

void PPCRecompiler_init();
void PPCRecompiler_Shutdown();

// enabling is counted: each Disable must be matched by an Enable
void PPCRecompiler_Enable();
void PPCRecompiler_Disable();

// makes guest code in the range eligible for recompilation
void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size);

// drops compiled code that covers any part of [startAddr, endAddr)
void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr);

// host stubs: entering generated code and returning to the interpreter
extern void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
extern void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
extern void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

// requests compilation of the code at the address if that did not happen yet
void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress);
// runs compiled code at the address if there is any (requests compilation otherwise)
void PPCRecompiler_attemptEnter(struct PPCInterpreter_t* hCPU, uint32 enterAddress);
// runs compiled code at the address if there is any
void PPCRecompiler_attemptEnterWithoutRecompile(struct PPCInterpreter_t* hCPU, uint32 enterAddress);
