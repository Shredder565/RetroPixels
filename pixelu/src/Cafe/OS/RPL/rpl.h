// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// The RPL/RPX loader: maps Cafe OS modules into guest memory, tracks which
// modules are needed (by name, with reference counts), links imports to
// exports of other modules or to the C++ (HLE) libraries and runs module
// entry points.
#pragma once

struct RPLModule;

#define RPL_INVALID_HANDLE		0xFFFFFFFF

void RPLLoader_InitState();
void RPLLoader_UnloadAll();

// guest code memory
uint8* RPLLoader_AllocateTrampolineCodeSpace(sint32 size);
MPTR RPLLoader_AllocateCodeSpace(uint32 size, uint32 alignment);
uint32 RPLLoader_GetMaxCodeOffset();
uint32 RPLLoader_GetDataAllocatorAddr();

// loading and linking
RPLModule* RPLLoader_LoadFromMemory(uint8* rplData, sint32 size, std::string_view name); // maps sections, does not link
uint32 rpl_mapHLEImport(RPLModule* rplLoaderContext, const char* rplName, const char* funcName, bool functionMustExist);
void RPLLoader_Link();

MPTR RPLLoader_FindRPLExport(RPLModule* rplLoaderContext, const char* symbolName, bool isData);
uint32 RPLLoader_GetModuleEntrypoint(RPLModule* rplLoaderContext);

void RPLLoader_SetMainModule(RPLModule* rplLoaderContext);
uint32 RPLLoader_GetMainModuleHandle();

void RPLLoader_CallEntrypoints();
void RPLLoader_CallCoreinitEntrypoint();
void RPLLoader_NotifyControlPassedToApplication(); // from now on module memory comes from the OSDynLoad allocator

// dependency tracking
void RPLLoader_AddDependency(std::string_view name, bool isMainExecutable = false);
void RPLLoader_RemoveDependency(uint32 handle);
bool RPLLoader_HasDependency(std::string_view name);
void RPLLoader_UpdateDependencies(); // loads newly requested modules, unloads unreferenced ones

void RPLLoader_LoadCoreinit();

uint32 RPLLoader_GetHandleByModuleName(const char* name);
const std::string RPLLoader_GetModuleNameByHandle(uint32 handle);
uint32 RPLLoader_GetMaxTLSModuleIndex();
bool RPLLoader_GetTLSDataByTLSIndex(sint16 tlsModuleIndex, uint8** tlsData, sint32* tlsSize);

uint32 RPLLoader_FindModuleOrHLEExport(uint32 moduleHandle, bool isData, const char* exportName);

uint32 RPLLoader_GetSDA1Base();
uint32 RPLLoader_GetSDA2Base();

std::span<RPLModule*> RPLLoader_GetModuleList();
RPLModule* RPLLoader_GetModuleByName(std::string_view name);

MEMPTR<void> RPLLoader_AllocateCodeCaveMem(uint32 alignment, uint32 size);
void RPLLoader_ReleaseCodeCaveMem(MEMPTR<void> addr);

// guest-callable address for a host function
uint32 RPLLoader_MakePPCCallable(void(*ppcCallableExport)(struct PPCInterpreter_t* hCPU));

// plain ELF (homebrew) loader
uint32 ELF_LoadFromMemory(uint8* elfData, sint32 size, const char* name);
