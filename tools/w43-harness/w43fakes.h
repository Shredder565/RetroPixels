// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// W43 harness: the fake neighbours' control surface and the trace recorder.
// Only the harness driver includes this; the device files see the real headers.

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace DiscIO
{
class Volume;
}

namespace w43
{
// ---- Hashing and traces --------------------------------------------------------------------

u64 Fnv(std::span<const u8> data, u64 h = 0xcbf29ce484222325ULL);
u64 FnvStr(std::string_view s, u64 h = 0xcbf29ce484222325ULL);
std::string Hex(std::span<const u8> data);
u64 Mix(u64 x);

// Behaviour trace line (device-visible effects) and log trace line.
void B(std::string line);
void L(std::string line);

struct CaseHashes
{
  u64 behaviour;
  u64 logs;
  u64 lines;
  u64 log_lines;
};

// Starts a case: clears traces and fake state. `mask_dir` is replaced by "$DIR" in every line.
// With `echo`, every line is also printed ("B ..." / "L ...").
void BeginCase(u64 seed, const std::string& mask_dir, bool echo);
CaseHashes EndCase();

// A small deterministic generator (splitmix64).
struct Rng
{
  u64 state;
  explicit Rng(u64 seed) : state(seed) {}
  u64 Next();
  u32 U32() { return static_cast<u32>(Next() >> 32); }
  u32 Below(u32 n) { return n == 0 ? 0 : static_cast<u32>(Next() % n); }
  bool Pct(u32 percent) { return Below(100) < percent; }
  u8 Byte() { return static_cast<u8>(Next() >> 56); }
  std::vector<u8> Bytes(std::size_t n);
};

// ---- Fake emulated memory (24 MiB, mirrored every 1 GiB like MEM1) ---------------------------

constexpr u32 RAM_SIZE = 0x01800000;
u8* Ram();
void RamWriteBE32(u32 address, u32 value);
u32 RamReadBE32(u32 address);
void RamWrite(u32 address, std::span<const u8> data);
std::vector<u8> RamRead(u32 address, u32 size);
u64 RamHash();

// ---- Fake inputs and neighbour state ---------------------------------------------------------

void SetPad(int pad, const GCPadStatus& status);
void SetSIDeviceType(int channel, int si_device_type);
void SetGameID(std::string id);

// CoreTiming: runs every scheduled event in order (each is traced).
void FireScheduledEvents();
std::size_t PendingEventCount();

// Sockets: the outcome generator for the fake socket layer (see sockets in fakes.cpp).
void SetSocketSeed(u64 seed);

// Tapserver: outcomes of Activate() and SendAndRemoveAllHDLCFrames(), and frame delivery.
void SetTapSeed(u64 seed);
bool TapDeliver(std::string frame);  // false when no connection exists
void TapForget();                    // the modem is gone

// DiscIO: a fake volume whose blob reader serves a deterministic pattern of `data_size` bytes.
const DiscIO::Volume& FakeVolume(u64 data_size);

}  // namespace w43
