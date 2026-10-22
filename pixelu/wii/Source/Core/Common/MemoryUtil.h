// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>

namespace Common
{
// Read-write-execute memory for JIT code. Panics and returns nullptr on failure.
void* AllocateExecutableMemory(size_t size);

// Per-thread write/execute switching for JIT memory, for platforms that forbid a thread from
// having both at once. The calls nest: execute is re-enabled only by the outermost
// JITPageWriteDisableExecuteEnable(). Prefer ScopedJITPageWriteAndNoExecute, which pairs them.
void JITPageWriteEnableExecuteDisable();
void JITPageWriteDisableExecuteEnable();

// While in scope, the thread may write JIT memory but not execute it.
struct ScopedJITPageWriteAndNoExecute
{
  ScopedJITPageWriteAndNoExecute() { JITPageWriteEnableExecuteDisable(); }
  ~ScopedJITPageWriteAndNoExecute() { JITPageWriteDisableExecuteEnable(); }
};

// Whole pages of read-write memory, and their release (size must be the allocated size).
void* AllocateMemoryPages(size_t size);
bool FreeMemoryPages(void* ptr, size_t size);

// Heap memory with the given power-of-two alignment; free with FreeAlignedMemory().
void* AllocateAlignedMemory(size_t size, size_t alignment);
void FreeAlignedMemory(void* ptr);

// Page protection changes. Each panics and returns false on failure.
bool ReadProtectMemory(void* ptr, size_t size);
bool WriteProtectMemory(void* ptr, size_t size, bool executable = false);
bool UnWriteProtectMemory(void* ptr, size_t size, bool allowExecute = false);

// Total installed physical memory in bytes.
size_t MemPhysical();

}  // namespace Common
