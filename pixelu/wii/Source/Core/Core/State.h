// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Savestates: saving the whole emulated machine to a file and loading it back.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <type_traits>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace State
{
// Number of numbered slots (1 to NUM_STATES).
static const u32 NUM_STATES = 10;

// The file layout, in order: StateHeaderLegacy, StateHeaderVersion, the
// version string, StateExtendedBaseHeader, then the payload. All of these
// structs are written to disk as raw bytes, so their layouts are fixed.

struct StateHeaderLegacy
{
  char game_id[6];
  char reserved1[2];
  // Non-zero only in very old LZO-compressed states, whose version header sits
  // inside the first compressed block.
  u32 lzo_size = 0;
  char reserved2[4];
  // Creation time (see the .cpp for the epoch used).
  double time;
};
constexpr size_t STATE_HEADER_SIZE = sizeof(StateHeaderLegacy);
static_assert(STATE_HEADER_SIZE == 24);
static_assert(offsetof(StateHeaderLegacy, lzo_size) == 8);
static_assert(offsetof(StateHeaderLegacy, time) == 16);
static_assert(std::is_trivially_copyable_v<StateHeaderLegacy>);

struct StateHeaderVersion
{
  u32 version_cookie;  // a magic base plus the state format version
  u32 version_string_length;
};
static_assert(std::is_trivially_copyable_v<StateHeaderVersion>);

struct StateHeader
{
  StateHeaderLegacy legacy_header;
  StateHeaderVersion version_header;
  std::string version_string;  // the build that wrote the state
};

// Stored as a number in the file: only ever append new values.
enum CompressionType : u16
{
  Uncompressed = 0,
  LZ4 = 1,
};

struct StateExtendedBaseHeader
{
  u16 header_version;
  u16 compression_type;  // a CompressionType
  u32 payload_offset;    // extra bytes between this header and the payload
  u64 uncompressed_size;
};
constexpr size_t EXTENDED_BASE_HEADER_SIZE = sizeof(StateExtendedBaseHeader);
static_assert(EXTENDED_BASE_HEADER_SIZE == 16);
static_assert(offsetof(StateExtendedBaseHeader, payload_offset) == 4);
static_assert(offsetof(StateExtendedBaseHeader, uncompressed_size) == 8);
static_assert(std::is_trivially_copyable_v<StateExtendedBaseHeader>);

// Room for future header fields after the base. Adding any means updating the
// header version and the code that writes and reads the headers.
struct StateExtendedHeader
{
  StateExtendedBaseHeader base_header;
};

void Init(Core::System& system);
void Shutdown();

// A label for the slot for the UI: its creation time, or "Empty"/"Unknown".
std::string GetInfoStringOfSlot(u32 slot, bool translate = true);

// When the slot's state was saved, in milliseconds since 1970; 0 if there is none.
u64 GetUnixTimeOfSlot(u32 slot);

// Saving and loading run on the CPU thread: immediately when called from it,
// otherwise queued to it.
void Save(Core::System& system, u32 slot);
void Load(Core::System& system, u32 slot);

void SaveAs(Core::System& system, std::string filename);
void LoadAs(Core::System& system, std::string filename);

// Loads the i-th entry of the used slots ordered newest first.
void LoadLastSaved(Core::System& system, int i = 1);
// Saves to the first empty slot, or over the oldest state if all are used.
void SaveFirstSaved(Core::System& system);
// Restores the state file that the last save replaced.
void UndoSaveState(Core::System& system);
// Returns to the emulation state from just before the last load.
void UndoLoadState(Core::System& system);

// Lets the UI react to a load without Core depending on it.
using AfterLoadCallbackFunc = std::function<void()>;
void SetOnAfterLoadCallback(AfterLoadCallbackFunc callback);
}  // namespace State
