// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// W43 old-vs-new harness driver. It drives seeded random sequences through the public interface
// of each W43 device, with every neighbour faked (fakes.cpp), and prints two hashes per case:
// "beh" over everything the device did to its neighbours (replies, memory, files, alerts,
// scheduled events, socket calls, savestate bytes) and "log" over its log output.
//
// It includes only the public headers of the W43 devices, so the same driver builds against the
// old and the new implementation. See docs/w43-spec.md section 16.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Network.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceAGP.h"
#include "Core/HW/EXI/EXI_DeviceBaseboard.h"
#include "Core/HW/EXI/EXI_DeviceModem.h"
#include "Core/HW/MagCard/C1231BR.h"
#include "Core/HW/MagCard/C1231LR.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceAMBaseboard.h"
#include "Core/HW/Triforce/DeckReader.h"
#include "Core/HW/Triforce/FZeroAX.h"
#include "Core/HW/Triforce/ICCardReader.h"
#include "Core/HW/Triforce/IOPorts.h"
#include "Core/HW/Triforce/JVSIO.h"
#include "Core/HW/Triforce/MarioKartGP.h"
#include "Core/HW/Triforce/Touchscreen.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

#include "w43fakes.h"

namespace w43
{
void ResetSockets();
}

namespace
{
using namespace w43;
namespace fs = std::filesystem;

Core::System& Sys()
{
  return Core::System::GetInstance();
}

// ---- Small helpers ---------------------------------------------------------------------------

std::string TriDir()
{
  return File::GetUserPath(D_TRIUSER_IDX);
}

void WriteFile(const std::string& path, std::span<const u8> data)
{
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void WriteText(const std::string& path, std::string_view text)
{
  WriteFile(path, {reinterpret_cast<const u8*>(text.data()), text.size()});
}

void Append(std::vector<u8>& v, std::span<const u8> d)
{
  v.insert(v.end(), d.begin(), d.end());
}

void PutLE32(std::span<u8> d, std::size_t off, u32 v)
{
  for (int i = 0; i < 4; ++i)
    d[off + i] = static_cast<u8>(v >> (8 * i));
}

u32 GetLE32(std::span<const u8> d, std::size_t off)
{
  return u32{d[off]} | (u32{d[off + 1]} << 8) | (u32{d[off + 2]} << 16) | (u32{d[off + 3]} << 24);
}

void RecordTx(const char* tag, Triforce::SerialDevice& dev)
{
  std::vector<u8> out(dev.GetTxByteCount());
  dev.TakeTxBytes(out);
  B(fmt::format("{} tx {}", tag, Hex(out)));
}

GCPadStatus RandomPad(Rng& r)
{
  GCPadStatus p;
  if (r.Pct(25))
    return p;
  p.button = static_cast<u16>(r.U32() & 0x1F7F);
  p.stickX = r.Byte();
  p.stickY = r.Byte();
  p.substickX = r.Byte();
  p.substickY = r.Byte();
  p.triggerLeft = r.Byte();
  p.triggerRight = r.Byte();
  p.switches = static_cast<u8>((r.Pct(12) ? 1 : 0) | (r.Pct(12) ? 2 : 0) | (r.Pct(12) ? 4 : 0));
  return p;
}

// Savestate round trip helpers: measure, write (traced), then read back.
template <typename F>
std::vector<u8> SaveState(F&& f)
{
  u8* ptr = nullptr;
  PointerWrap pm(&ptr, 0, PointerWrap::Mode::Measure);
  f(pm);
  const std::size_t size = static_cast<std::size_t>(ptr - static_cast<u8*>(nullptr));
  std::vector<u8> buf(size);
  u8* wp = buf.data();
  PointerWrap pw(&wp, size, PointerWrap::Mode::Write);
  f(pw);
  B(fmt::format("STATE save size={} hash={:016x} ok={}", size, Fnv(buf), !pw.IsMeasureMode()));
  return buf;
}

template <typename F>
void LoadState(std::vector<u8>& buf, F&& f)
{
  u8* rp = buf.data();
  PointerWrap pr(&rp, buf.size(), PointerWrap::Mode::Read);
  f(pr);
  B(fmt::format("STATE load consumed={} ok={}", rp - buf.data(), !pr.IsMeasureMode()));
}

// ================================================================================================
// Message generators
// ================================================================================================

// ---- JVS ---------------------------------------------------------------------------------------

struct JvsModel
{
  u8 address = 0;
};

void JvsEscape(std::vector<u8>& out, u8 b)
{
  if (b == 0xE0 || b == 0xD0)
  {
    out.push_back(0xD0);
    out.push_back(static_cast<u8>(b - 1));
  }
  else
  {
    out.push_back(b);
  }
}

// Returns the wire bytes; `ok` says whether the checksum is valid.
std::vector<u8> JvsFrame(Rng& r, u8 dest, const std::vector<u8>& payload, bool* ok)
{
  std::vector<u8> body{dest, static_cast<u8>(payload.size() + 1)};
  Append(body, payload);
  u8 sum = 0;
  for (const u8 b : body)
    sum = static_cast<u8>(sum + b);
  *ok = true;
  if (r.Pct(6))
  {
    sum = static_cast<u8>(sum ^ (1 + r.Below(255)));
    *ok = false;
  }
  body.push_back(sum);
  std::vector<u8> out{0xE0};
  for (const u8 b : body)
    JvsEscape(out, b);
  return out;
}

u8 OutputByte(Rng& r)
{
  static constexpr u8 interesting[] = {0x00, 0x40, 0x10, 0x50, 0x80, 0x20, 0xA0, 0xFF,
                                       0x04, 0x08, 0x0C, 0xC0, 0xE0, 0x30};
  return r.Pct(70) ? interesting[r.Below(sizeof(interesting))] : r.Byte();
}

// Returns false when the command must be the last one of the frame (truncated parameters:
// a following command byte would be read as a parameter, e.g. a player count above 2).
bool AddJvsCommand(Rng& r, std::vector<u8>& p)
{
  switch (r.Below(22))
  {
  case 0:
    p.push_back(0x10);
    break;
  case 1:
    p.push_back(0x11);
    break;
  case 2:
    p.push_back(0x12);
    break;
  case 3:
    p.push_back(0x13);
    break;
  case 4:
    p.push_back(0x14);
    break;
  case 5:
  {
    p.push_back(0x15);
    const u32 n = r.Below(12);
    for (u32 i = 0; i < n; ++i)
      p.push_back(static_cast<u8>('A' + r.Below(26)));
    if (r.Pct(85))
      p.push_back(0);
    break;
  }
  case 6:
  case 7:
  case 8:
    p.push_back(0x20);
    p.push_back(static_cast<u8>(r.Below(3)));
    p.push_back(static_cast<u8>(r.Below(5)));
    break;
  case 9:
    p.push_back(0x21);
    p.push_back(static_cast<u8>(r.Below(5)));
    break;
  case 10:
    p.push_back(0x22);
    p.push_back(static_cast<u8>(r.Below(11)));
    break;
  case 11:
    p.push_back(r.Pct(50) ? 0x30 : 0x35);
    p.push_back(static_cast<u8>(r.Below(4)));
    p.push_back(static_cast<u8>(r.Pct(80) ? 0 : r.Byte()));
    p.push_back(r.Byte());
    break;
  case 12:
  case 13:
  case 14:
  {
    p.push_back(0x32);
    const u32 k = r.Below(7);
    p.push_back(static_cast<u8>(k));
    for (u32 i = 0; i < k; ++i)
      p.push_back(OutputByte(r));
    break;
  }
  case 15:
    p.push_back(0x70);
    p.push_back(r.Pct(70) ? 0x18 : r.Byte());
    for (int i = 0; i < 4; ++i)
      p.push_back(r.Byte());
    break;
  case 16:
  {
    static constexpr u8 unknown[] = {0x23, 0x24, 0x2E, 0x31, 0x33, 0x40, 0xF2, 0x99, 0x00};
    p.push_back(unknown[r.Below(sizeof(unknown))]);
    break;
  }
  case 17:
    p.push_back(0x2F);
    break;
  case 18:
  {
    // Truncated parameters (the rest of the frame is cut off).
    static constexpr u8 needs_params[] = {0x20, 0x21, 0x22, 0x30, 0x35, 0x32, 0x70};
    p.push_back(needs_params[r.Below(sizeof(needs_params))]);
    if (r.Pct(50))
      p.push_back(static_cast<u8>(r.Below(3)));
    return false;
  }
  default:
    p.push_back(0x20);
    p.push_back(0x02);
    p.push_back(0x02);
    break;
  }
  return true;
}

std::vector<u8> GenJvs(Rng& r, JvsModel& m)
{
  const u32 kind = r.Below(100);
  if (kind < 4)
    return {static_cast<u8>(r.Pct(50) ? 0x55 : r.Byte() & 0x7F)};
  if (kind < 7)
  {
    std::vector<u8> out{0xE0};
    JvsEscape(out, r.Pct(50) ? m.address : u8{0xFF});
    out.push_back(0x00);
    return out;
  }
  bool ok = false;
  if (kind < 22)
  {
    std::vector<u8> payload;
    u8 new_address = 0;
    int effect = 0;
    switch (r.Below(7))
    {
    case 0:
    case 1:
      payload = {0xF0, 0xD9};
      effect = 1;
      break;
    case 2:
      payload = {0xF0, r.Byte()};
      if (payload[1] == 0xD9)
        effect = 1;
      break;
    case 3:
      payload = {0xF0};
      break;
    case 4:
    case 5:
      new_address = r.Pct(80) ? 1 : static_cast<u8>(r.Below(4));
      payload = {0xF1, new_address};
      effect = 2;
      break;
    default:
      payload = {static_cast<u8>(r.Pct(50) ? 0xF1 : 0x10)};
      if (payload[0] == 0x10 && r.Pct(50))
        payload.push_back(0x11);
      break;
    }
    auto out = JvsFrame(r, 0xFF, payload, &ok);
    if (ok && effect == 1)
      m.address = 0;
    if (ok && effect == 2)
      m.address = new_address;
    return out;
  }
  const u8 dest = r.Pct(90) ? m.address : static_cast<u8>(r.Below(4));
  std::vector<u8> payload;
  if (r.Pct(5))
  {
    payload.push_back(0x2F);
  }
  else
  {
    const u32 n = 1 + r.Below(r.Pct(10) ? 12 : 4);
    for (u32 i = 0; i < n && payload.size() < 200; ++i)
    {
      if (!AddJvsCommand(r, payload))
        break;
    }
  }
  return JvsFrame(r, dest, payload, &ok);
}

// ---- Wheels and touchscreen -----------------------------------------------------------------

std::vector<u8> GenFzWheel(Rng& r)
{
  static constexpr u8 cmds[] = {0, 1, 2, 4, 4, 4, 6, 7, 9, 0x7F, 0x7F};
  u8 cmd = r.Pct(90) ? cmds[r.Below(sizeof(cmds))] : r.Byte();
  std::vector<u8> req{static_cast<u8>(cmd ^ 0x80), static_cast<u8>(r.Below(2)), r.Byte(), 0};
  req[3] = static_cast<u8>(req[0] ^ req[1] ^ req[2] ^ 0x80);
  if (r.Pct(6))
    req[3] = static_cast<u8>(req[3] ^ 0x21);
  if (r.Pct(4))
    req.insert(req.begin(), r.Byte());
  return req;
}

std::vector<u8> GenMkWheel(Rng& r)
{
  std::vector<u8> req{0xFF, 0xFF, 0xFF, 0x01};
  if (r.Pct(8))
    req[r.Below(4)] = r.Byte();
  Append(req, r.Bytes(6));
  return req;
}

std::vector<u8> GenTouch(Rng& r)
{
  return r.Bytes(1 + r.Below(5));
}

// ---- IC card ---------------------------------------------------------------------------------

std::vector<u8> IcFrame(Rng& r, u8 cmd, const std::vector<u8>& payload)
{
  std::vector<u8> f{0x00, cmd, static_cast<u8>(payload.size() >> 8),
                    static_cast<u8>(payload.size())};
  Append(f, payload);
  u8 x = 0;
  for (const u8 b : f)
    x ^= b;
  if (r.Pct(4))
    x ^= 0x5A;
  f.push_back(x);
  return f;
}

void PutBE16(std::vector<u8>& v, std::size_t off, u16 value)
{
  v[off] = static_cast<u8>(value >> 8);
  v[off + 1] = static_cast<u8>(value);
}

u16 IcSession(Rng& r)
{
  if (r.Pct(85))
    return static_cast<u16>(0x2300 | (r.Pct(85) ? 0 : r.Below(3)));
  return static_cast<u16>(r.U32());
}

u16 IcPage(Rng& r)
{
  static constexpr u16 pages[] = {0, 1, 3, 4, 5, 6, 0x28 / 8, 100, 254, 255, 0x104};
  return r.Pct(70) ? pages[r.Below(sizeof(pages) / sizeof(pages[0]))] :
                     static_cast<u16>(r.Below(256));
}

std::vector<u8> GenIc(Rng& r, u8 slot)
{
  static constexpr u8 cmds[] = {0x10, 0x11, 0x14, 0x15, 0x16, 0x20, 0x20, 0x21, 0x21, 0x22,
                                0x22, 0x24, 0x24, 0x25, 0x26, 0x27, 0x33, 0x34, 0x34, 0x35,
                                0x35, 0x12, 0x99};
  const u8 cmd = cmds[r.Below(sizeof(cmds))];
  std::vector<u8> p;
  switch (cmd)
  {
  case 0x11:
  case 0x20:
    p = r.Bytes(8);
    if (cmd == 0x20)
      PutBE16(p, 0, static_cast<u16>(r.Below(2)));
    break;
  case 0x21:
    p = r.Bytes(16);
    break;
  case 0x22:
    p = {0x00, 0x00, 0x54, 0x4D, 0x50, 0x00, slot, static_cast<u8>(r.Pct(80) ? 0 : r.Below(3))};
    if (r.Pct(10))
      p[r.Below(8)] = r.Byte();
    break;
  case 0x24:
  case 0x33:
  case 0x27:
    p.assign(8, 0);
    PutBE16(p, 0, IcSession(r));
    PutBE16(p, 2, IcPage(r));
    break;
  case 0x25:
    p = r.Bytes(16);
    PutBE16(p, 0, IcSession(r));
    PutBE16(p, 2, 0);
    PutBE16(p, 4, IcPage(r));
    break;
  case 0x26:
    p.assign(8, 0);
    PutBE16(p, 0, IcSession(r));
    PutBE16(p, 2, static_cast<u16>(r.Pct(80) ? 5 : IcPage(r)));
    PutBE16(p, 4, static_cast<u16>(r.Pct(80) ? 1 : r.U32()));
    break;
  case 0x34:
    p.assign(8, 0);
    PutBE16(p, 0, IcSession(r));
    PutBE16(p, 2, IcPage(r));
    PutBE16(p, 4, static_cast<u16>(r.Pct(80) ? r.Below(5) : r.Below(400)));
    break;
  case 0x35:
  {
    const u16 count = static_cast<u16>(r.Below(4));
    p = r.Bytes(8 + count * 8u);
    PutBE16(p, 0, IcSession(r));
    PutBE16(p, 2, IcPage(r));
    PutBE16(p, 4, count);
    if (r.Pct(10))
      p.resize(r.Below(p.size()));
    break;
  }
  default:
    break;
  }
  if (r.Pct(5))
    p.resize(p.size() + 1 + r.Below(8), 0xEE);  // oversized
  else if (r.Pct(5) && !p.empty())
    p.resize(r.Below(p.size()));  // undersized
  return IcFrame(r, cmd, p);
}

// ---- Deck reader -----------------------------------------------------------------------------

struct DeckModel
{
  int firmware_left = 0;  // messages still sent as firmware data
};

std::vector<u8> GenDeck(Rng& r, DeckModel& m)
{
  if (m.firmware_left > 0)
  {
    --m.firmware_left;
    return r.Bytes(1 + r.Below(48));
  }
  if (r.Pct(35))
    return GenIc(r, 0);
  static constexpr u8 cmds[] = {0x74, 0x63, 0x76, 0x62, 0x6B, 0x6D, 0x68, 0x67, 0x61, 0x69,
                                0x6C, 0x6F, 0x73, 0x65, 0x72, 0x72, 0x72, 0x66, 0x55, 0x10};
  const u8 cmd = cmds[r.Below(sizeof(cmds))];
  if (cmd == 0x66)
    m.firmware_left = 1 + static_cast<int>(r.Below(4));
  return {cmd};
}

// ---- Magnetic card reader --------------------------------------------------------------------

std::vector<u8> MagPacket(Rng& r, u8 cmd, const std::vector<u8>& params)
{
  std::vector<u8> p{cmd, 0x30, 0x30, 0x30};
  Append(p, params);
  p.push_back(0x03);
  const u8 size = static_cast<u8>(p.size() + 1);
  u8 x = size;
  for (const u8 b : p)
    x ^= b;
  p.push_back(x);
  const u32 corrupt = r.Below(100);
  if (corrupt < 3)
    p.back() ^= 0x11;
  else if (corrupt < 5)
    p[p.size() - 2] = 0x04;
  std::vector<u8> out{0x02, static_cast<u8>(p.size())};
  if (corrupt >= 5 && corrupt < 7)
  {
    // Too short to be a packet.
    return {0x02, 0x03, cmd, 0x03, 0x00};
  }
  Append(out, p);
  return out;
}

std::vector<u8> GenMag(Rng& r)
{
  const u32 k = r.Below(100);
  if (k < 55)
    return {0x05};
  if (k < 58)
    return {static_cast<u8>(r.Pct(50) ? 0x06 : r.Byte())};
  static constexpr u8 cmds[] = {0x10, 0x20, 0x20, 0x33, 0x33, 0x33, 0x35, 0x40, 0x53, 0x53,
                                0x78, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x80, 0x80, 0xA0, 0xB0,
                                0xB0, 0xC0, 0xC1, 0xD0, 0xD0, 0xF0, 0x99};
  const u8 cmd = cmds[r.Below(sizeof(cmds))];
  std::vector<u8> params;
  auto track = [&] {
    return static_cast<u8>(r.Pct(90) ? 0x30 + r.Below(7) : r.Byte());
  };
  switch (cmd)
  {
  case 0x10:
    if (r.Pct(80))
      params = {static_cast<u8>(0x30 + r.Below(3))};
    break;
  case 0x33:
    params = {static_cast<u8>(0x30 + r.Below(3)), 0x30, track()};
    if (r.Pct(10))
      params.resize(r.Below(3));
    break;
  case 0x53:
  {
    const u8 t = track();
    params = {static_cast<u8>(r.Pct(90) ? 0x30 : 0x31), 0x30, t};
    std::size_t tracks = 0;
    if (t == 0x30)
      tracks = 1;
    else if (t == 0x33)
      tracks = 2;
    else if (t == 0x36)
      tracks = 3;
    // Tracks 31, 32, 34, 35 with a matching length hit undefined behaviour in the old code
    // (docs/w43-spec.md section 13), so they are only sent with a wrong length.
    std::size_t len = tracks * 69;
    if (tracks == 0 || r.Pct(10))
      len = 69 * (1 + r.Below(2)) + 1;
    Append(params, r.Bytes(len));
    if (r.Pct(5))
      params.resize(r.Below(3));
    break;
  }
  case 0x7C:
    params = {0x30, 0x30, 0x30};
    Append(params, r.Bytes(r.Below(10)));
    if (r.Pct(15))
      params.resize(r.Below(3));
    break;
  case 0xB0:
    if (r.Pct(70))
      params = {static_cast<u8>(r.Pct(50) ? 0x31 : 0x32)};
    break;
  case 0xC0:
  case 0xC1:
    params = {static_cast<u8>(0x30 + r.Below(4))};
    break;
  case 0xD0:
    if (r.Pct(85))
      params = {static_cast<u8>(r.Pct(50) ? 0x30 : 0x31)};
    break;
  default:
    if (r.Pct(20))
      params = r.Bytes(r.Below(6));
    break;
  }
  return MagPacket(r, cmd, params);
}

// ---- Card files ------------------------------------------------------------------------------

void MaybeCreateMagFile(Rng& r, const std::string& path)
{
  static constexpr std::size_t sizes[] = {207, 207, 138, 69, 0, 100, 300};
  if (r.Pct(40))
    return;
  WriteFile(path, r.Bytes(sizes[r.Below(7)]));
}

void MaybeCreateIcFiles(Rng& r, const std::string& game_id)
{
  for (int slot = 1; slot <= 2; ++slot)
  {
    if (r.Pct(50))
      continue;
    static constexpr std::size_t sizes[] = {2048, 2048, 100, 3000, 0};
    WriteFile(fmt::format("{}tricard_{}_slot{}.bin", TriDir(), game_id, slot),
              r.Bytes(sizes[r.Below(5)]));
  }
}

void MaybeCreateDeckFile(Rng& r, const std::string& game_id)
{
  const std::string path = fmt::format("{}tricard_{}_deck.json", TriDir(), game_id);
  switch (r.Below(6))
  {
  case 0:
    return;
  case 1:
    WriteText(path, "{\"cards\": [ {\"index\": 3}, ");  // parse error
    return;
  case 2:
    WriteText(path, "[1, 2, 3]");
    return;
  case 3:
    WriteText(path, "{\"cards\": 5}");
    return;
  default:
    break;
  }
  std::string text = "{\"cards\": [";
  const u32 n = r.Below(r.Pct(10) ? 40 : 8);
  for (u32 i = 0; i < n; ++i)
  {
    if (i != 0)
      text += ",";
    switch (r.Below(8))
    {
    case 0:
      text += "\"not an object\"";
      break;
    case 1:
      text += "{\"table\": 1}";
      break;
    case 2:
      text += fmt::format("{{\"index\": {}, \"quantity\": {}}}", r.Below(500), r.Below(4));
      break;
    case 3:
      text += fmt::format("{{\"index\": {}, \"table\": {}}}", r.Below(65536), r.Below(2));
      break;
    case 4:
      text += "{\"index\": \"12\"}";
      break;
    default:
      text += fmt::format("{{\"index\": {}}}", r.Below(1000));
      break;
    }
  }
  text += "]}";
  WriteText(path, text);
}

// ================================================================================================
// Cases: JVS boards
// ================================================================================================

class HarnessAdapter final : public Triforce::IOAdapter
{
public:
  explicit HarnessAdapter(u64 seed) : m_rng(seed) {}

  void Update() override
  {
    auto* io = GetIOPorts();
    *io->GetSystemInputs() = m_rng.Pct(50) ? m_rng.Byte() : u8{0};
    for (u32 p = 0; p < 2; ++p)
    {
      for (auto& b : io->GetSwitchInputs(p))
        b = m_rng.Pct(50) ? m_rng.Byte() : u8{0};
    }
    for (auto& a : io->GetAnalogInputs())
    {
      if (m_rng.Pct(60))
        a = static_cast<u16>(m_rng.U32());
    }
    for (bool& c : io->GetCoinInputs())
      c = m_rng.Pct(30);
  }

protected:
  void HandleGenericOutputsChanged(std::span<const u8> bits_set,
                                   std::span<const u8> bits_cleared) override
  {
    B(fmt::format("ADAPTER.outputs set={} clr={}", Hex(bits_set), Hex(bits_cleared)));
  }

private:
  Rng m_rng;
};

void RecordPorts(const Triforce::IOPorts& ports)
{
  B(fmt::format("PORTS st={} go={}", Hex(ports.GetStatusSwitches()),
                Hex(ports.GetGenericOutputs())));
}

std::unique_ptr<Triforce::JVSIOBoard> MakeJvs(int board, Triforce::IOPorts* ports)
{
  switch (board)
  {
  case 0:
    return std::make_unique<Triforce::Namco_FCA_JVSIOBoard>(ports);
  case 1:
    return std::make_unique<Triforce::Sega_837_13551_JVSIOBoard>(ports);
  default:
    return std::make_unique<Triforce::Sega_837_13844_JVSIOBoard>(ports);
  }
}

void CaseJvs(Rng& r, int board)
{
  Triforce::IOPorts ports;
  ports.AddIOAdapter(std::make_unique<HarnessAdapter>(r.Next()));
  auto jvs = MakeJvs(board, &ports);
  JvsModel model;
  std::vector<u8> pending;
  for (int op = 0; op < 400; ++op)
  {
    if (r.Pct(3))
    {
      auto buf = SaveState([&](PointerWrap& p) {
        ports.DoState(p);
        jvs->DoState(p);
      });
      auto fresh = MakeJvs(board, &ports);
      LoadState(buf, [&](PointerWrap& p) {
        ports.DoState(p);
        fresh->DoState(p);
      });
      jvs = std::move(fresh);
      continue;
    }
    if (r.Pct(4))
    {
      // Direct output writes (as other boards would do through IOPorts).
      std::vector<u8> out = r.Bytes(r.Below(7));
      ports.SetGenericOutputs(out);
      RecordPorts(ports);
      continue;
    }
    ports.Update();
    if (pending.empty())
      pending = GenJvs(r, model);
    const std::size_t n = r.Pct(75) ? pending.size() : 1 + r.Below(static_cast<u32>(pending.size()));
    jvs->WriteRxBytes({pending.data(), n});
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n));
    jvs->Update();
    RecordTx("JVS", *jvs);
    RecordPorts(ports);
  }
}

// ================================================================================================
// Cases: SI baseboard
// ================================================================================================

enum class Dev
{
  None,
  FzWheel,
  MkWheel,
  Ic0,
  Ic1,
  Deck,
  Touch,
  Mag,
};

struct GameDef
{
  const char* kind;
  const char* id;
  Dev a;
  Dev b;
};

constexpr GameDef GAMES[] = {
    {"si-fzeroax", "GGGE8P", Dev::FzWheel, Dev::Mag},
    {"si-fzeromonster", "GHAE8P", Dev::FzWheel, Dev::None},
    {"si-mkgp", "GKPJ8P", Dev::MkWheel, Dev::Mag},
    {"si-mkgp2", "GNJJ8P", Dev::MkWheel, Dev::Mag},
    {"si-vs3", "GEJJ8P", Dev::None, Dev::None},
    {"si-vs4", "GHJJ8P", Dev::Ic0, Dev::Ic1},
    {"si-vs4-2006", "GLJJ8P", Dev::Ic0, Dev::Ic1},
    {"si-gekitou", "GGXJ8P", Dev::Ic0, Dev::Ic1},
    {"si-avalon", "GFXJ8P", Dev::Deck, Dev::Touch},
    {"si-firmware", "G00J8P", Dev::None, Dev::None},
    {"si-unknownid", "GZZE01", Dev::None, Dev::None},
    {"si-shortid", "ABC", Dev::None, Dev::None},
};

struct SerialStream
{
  Dev dev = Dev::None;
  std::vector<u8> pending;
  DeckModel deck;

  void Refill(Rng& r)
  {
    switch (dev)
    {
    case Dev::FzWheel:
      pending = GenFzWheel(r);
      break;
    case Dev::MkWheel:
      pending = GenMkWheel(r);
      break;
    case Dev::Ic0:
      pending = GenIc(r, 0);
      break;
    case Dev::Ic1:
      pending = GenIc(r, 1);
      break;
    case Dev::Deck:
      pending = GenDeck(r, deck);
      break;
    case Dev::Touch:
      pending = GenTouch(r);
      break;
    case Dev::Mag:
      pending = GenMag(r);
      break;
    case Dev::None:
      pending = r.Bytes(1 + r.Below(6));
      break;
    }
  }

  std::vector<u8> Take(Rng& r, std::size_t max)
  {
    if (pending.empty())
      Refill(r);
    std::size_t n = std::min(pending.size(), max);
    if (r.Pct(15) && n > 1)
      n = 1 + r.Below(static_cast<u32>(n));
    std::vector<u8> out(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n));
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
  }
};

std::vector<u8> BuildSIRequest(Rng& r, SerialStream& a, SerialStream& b, std::vector<u8>& jvs,
                               JvsModel& jvs_model)
{
  std::vector<u8> c;
  const u32 count = 1 + r.Below(7);
  constexpr std::size_t max_len = 126;
  for (u32 i = 0; i < count; ++i)
  {
    const std::size_t room = max_len - c.size();
    if (room < 3)
      break;
    std::vector<u8> sub;
    switch (r.Below(26))
    {
    case 0:
      sub = {0x10, r.Byte()};
      break;
    case 1:
      sub = {0x11, r.Byte()};
      break;
    case 2:
      sub = {0x12, r.Byte(), r.Byte()};
      break;
    case 3:
      sub = {0x14, r.Byte(), r.Byte()};
      break;
    case 4:
      sub = {0x15, r.Byte()};
      break;
    case 5:
      sub = {0x16, r.Byte()};
      break;
    case 6:
      sub = {0x1F};
      Append(sub, r.Bytes(5));
      break;
    case 7:
      sub = {0x21};
      Append(sub, r.Bytes(4));
      break;
    case 8:
    {
      const u8 n = static_cast<u8>(r.Below(9));
      sub = {0x22, n};
      Append(sub, r.Bytes(std::max<std::size_t>(6, n)));
      break;
    }
    case 9:
      sub = {0x23, r.Byte(), r.Byte()};
      break;
    case 10:
      sub = {0x24, r.Byte(), r.Byte()};
      break;
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    {
      SerialStream& s = (r.Pct(50)) ? a : b;
      const u8 cmd = (&s == &a) ? 0x31 : 0x32;
      auto data = s.Take(r, std::min<std::size_t>(room - 2, 48));
      sub = {cmd, static_cast<u8>(data.size())};
      Append(sub, data);
      break;
    }
    case 17:
    case 18:
    case 19:
    case 20:
    {
      if (jvs.empty())
        jvs = GenJvs(r, jvs_model);
      std::size_t n = std::min(jvs.size(), std::min<std::size_t>(room - 2, 80));
      if (r.Pct(10) && n > 1)
        n = 1 + r.Below(static_cast<u32>(n));
      sub = {static_cast<u8>(r.Pct(50) ? 0x40 : 0x41), static_cast<u8>(n)};
      sub.insert(sub.end(), jvs.begin(), jvs.begin() + static_cast<std::ptrdiff_t>(n));
      jvs.erase(jvs.begin(), jvs.begin() + static_cast<std::ptrdiff_t>(n));
      break;
    }
    case 21:
    {
      const u8 n = static_cast<u8>(r.Below(5));
      sub = {0x60, n};
      Append(sub, r.Bytes(std::max<std::size_t>(2, n)));
      break;
    }
    case 22:
    {
      static constexpr u8 unknown[] = {0x00, 0x13, 0x17, 0x30, 0x50, 0x70, 0xFF};
      sub = {unknown[r.Below(sizeof(unknown))]};
      Append(sub, r.Bytes(r.Below(8)));
      break;
    }
    default:
      sub = {0x10, 0x00};
      break;
    }
    if (sub.size() > room)
      break;
    Append(c, sub);
  }
  return c;
}

void CaseSI(Rng& r, const GameDef& game)
{
  SetGameID(game.id);
  SetSIDeviceType(0, SerialInterface::SIDEVICE_AM_BASEBOARD);
  SetSIDeviceType(1, r.Pct(50) ? SerialInterface::SIDEVICE_AM_BASEBOARD :
                                 SerialInterface::SIDEVICE_NONE);
  const std::string mag_path = fmt::format("{}tricard_{}.bin", TriDir(), game.id);
  MaybeCreateMagFile(r, mag_path);
  MaybeCreateIcFiles(r, game.id);
  MaybeCreateDeckFile(r, game.id);

  auto make = [] {
    return std::make_unique<SerialInterface::CSIDevice_AMBaseboard>(
        Sys(), SerialInterface::SIDEVICE_AM_BASEBOARD, 0);
  };
  auto dev = make();
  SerialStream a{game.a};
  SerialStream b{game.b};
  std::vector<u8> jvs;
  JvsModel jvs_model;

  for (int op = 0; op < 320; ++op)
  {
    if (r.Pct(30))
      SetPad(0, RandomPad(r));
    if (r.Pct(20))
      SetPad(1, RandomPad(r));
    if (r.Pct(2))
    {
      auto buf = SaveState([&](PointerWrap& p) { dev->DoState(p); });
      auto fresh = make();
      LoadState(buf, [&](PointerWrap& p) { fresh->DoState(p); });
      dev = std::move(fresh);
      continue;
    }
    if (r.Pct(2))
    {
      if (r.Pct(50))
        fs::remove(mag_path);
      else
        WriteFile(mag_path, r.Bytes(207));
      B("FILE.toggle mag");
      continue;
    }
    if (r.Pct(1))
    {
      u32 hi = 0, lo = 0;
      const auto resp = dev->GetData(hi, lo);
      dev->SendCommand(r.U32(), r.Byte());
      B(fmt::format("SI.getdata {} {:08x} {:08x}", static_cast<int>(resp), hi, lo));
      continue;
    }

    std::array<u8, 128> buf{};
    int request_length = 0;
    const u32 k = r.Below(100);
    if (k < 3)
    {
      buf[0] = 0x00;
      request_length = 1;
    }
    else if (k < 4)
    {
      buf[0] = static_cast<u8>(r.Pct(50) ? 0x40 : 0xFF);
      request_length = 3;
    }
    else
    {
      const auto cmds = BuildSIRequest(r, a, b, jvs, jvs_model);
      buf[0] = 0x70;
      buf[1] = static_cast<u8>(cmds.size());
      std::ranges::copy(cmds, buf.begin() + 2);
      request_length = static_cast<int>(2 + cmds.size());
      if (r.Pct(3))
        request_length -= 1 + static_cast<int>(r.Below(2));
      else if (r.Pct(5))
        request_length = 128;
    }
    const std::string request = Hex({buf.data(), static_cast<std::size_t>(std::max(
                                                     2, std::min(request_length, 128)))});
    const int ret = dev->RunBuffer(buf.data(), request_length);
    B(fmt::format("SI.run req={} ret={} buf={}", request, ret, Hex(buf)));
  }
  dev.reset();
}

// ================================================================================================
// Cases: standalone serial devices
// ================================================================================================

template <typename Dev_, typename Gen>
void DriveSerial(Rng& r, std::unique_ptr<Dev_>& dev, Gen&& gen, int ops,
                 const std::function<std::unique_ptr<Dev_>()>& make, const char* tag,
                 const std::function<bool(Rng&)>& extra = nullptr)
{
  std::vector<u8> pending;
  for (int op = 0; op < ops; ++op)
  {
    if (r.Pct(3))
    {
      auto buf = SaveState([&](PointerWrap& p) { static_cast<Triforce::SerialDevice&>(*dev).DoState(p); });
      auto fresh = make();
      LoadState(buf, [&](PointerWrap& p) { static_cast<Triforce::SerialDevice&>(*fresh).DoState(p); });
      dev = std::move(fresh);
      continue;
    }
    if (extra && extra(r))
      continue;
    if (r.Pct(25))
      SetPad(0, RandomPad(r));
    if (pending.empty() && r.Pct(85))
      pending = gen(r);
    const std::size_t n =
        pending.empty() ? 0 :
                          (r.Pct(75) ? pending.size() : 1 + r.Below(static_cast<u32>(pending.size())));
    dev->WriteRxBytes({pending.data(), n});
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(n));
    dev->Update();
    RecordTx(tag, *dev);
  }
}

void CaseFzWheel(Rng& r)
{
  std::function<std::unique_ptr<Triforce::FZeroAXSteeringWheel>()> make = [] {
    return std::make_unique<Triforce::FZeroAXSteeringWheel>();
  };
  auto dev = make();
  DriveSerial(r, dev, GenFzWheel, 300, make, "FZW", [&](Rng& rr) {
    if (!rr.Pct(10))
      return false;
    B(fmt::format("FZW.state init={} servo={}", dev->IsInitializing(),
                  static_cast<int>(dev->GetServoPosition())));
    return true;
  });
}

void CaseMkWheel(Rng& r)
{
  std::function<std::unique_ptr<Triforce::MarioKartGPSteeringWheel>()> make = [] {
    return std::make_unique<Triforce::MarioKartGPSteeringWheel>();
  };
  auto dev = make();
  DriveSerial(r, dev, GenMkWheel, 300, make, "MKW", [&](Rng& rr) {
    if (!rr.Pct(5))
      return false;
    dev->Reset();
    return true;
  });
}

void CaseTouch(Rng& r)
{
  std::function<std::unique_ptr<Triforce::Touchscreen>()> make = [] {
    return std::make_unique<Triforce::Touchscreen>();
  };
  auto dev = make();
  DriveSerial(r, dev, [](Rng& rr) { return rr.Pct(20) ? GenTouch(rr) : std::vector<u8>{}; }, 200,
              make, "TOUCH");
}

const char* const IC_GAMES[] = {"GFXJ8P", "GJNJ8P", "GHJJ8P", "GLJJ8P", "GGXJ8P", "GEJJ8P"};

// A savestate built from the spec's layout (section 7.6), to reach multi-tag cards.
std::vector<u8> CraftIcState(Rng& r, u8 slot, u8 cards)
{
  std::vector<u8> s;
  auto u32le = [&](u32 v) {
    for (int i = 0; i < 4; ++i)
      s.push_back(static_cast<u8>(v >> (8 * i)));
  };
  u32le(0);  // rx bytes
  u32le(0);  // tx bytes
  s.push_back(cards);
  for (u8 i = 0; i < cards; ++i)
  {
    Append(s, r.Bytes(2048));
    const std::array<u8, 8> uid{0x00, 0x00, 0x54, 0x4D, 0x50, 0x00, slot, i};
    Append(s, uid);
    s.push_back(static_cast<u8>(r.Pct(70) ? 0 : 1));
  }
  s.push_back(static_cast<u8>(r.Below(2)));
  s.push_back(static_cast<u8>(r.Pct(50) ? 0 : r.Below(130)));
  s.push_back(static_cast<u8>(r.Below(12)));
  return s;
}

void CaseIc(Rng& r)
{
  const char* id = IC_GAMES[r.Below(6)];
  SetGameID(id);
  MaybeCreateIcFiles(r, id);
  const u8 slot = static_cast<u8>(r.Below(2));
  std::function<std::unique_ptr<Triforce::ICCardReader>()> make = [slot] {
    return std::make_unique<Triforce::ICCardReader>(slot);
  };
  auto dev = make();
  DriveSerial(r, dev, [slot](Rng& rr) { return GenIc(rr, slot); }, 400, make, "IC",
              [&](Rng& rr) {
                const u32 k = rr.Below(100);
                if (k < 6)
                  dev->InsertCard();
                else if (k < 9)
                  dev->EjectCard();
                else if (k < 14)
                  B(fmt::format("IC.state present={} ejecting={} ready={} checks={}",
                                dev->IsCardPresent(), dev->IsEjecting(),
                                dev->IsReadyToInsertCard(), dev->GetCardPresentInsertCheckCount()));
                else if (k < 15)
                {
                  auto state = CraftIcState(rr, slot, static_cast<u8>(1 + rr.Below(3)));
                  LoadState(state, [&](PointerWrap& p) { dev->DoState(p); });
                }
                else
                  return false;
                return true;
              });
}

void CaseDeck(Rng& r)
{
  const char* id = r.Pct(50) ? "GFXJ8P" : "GJNJ8P";
  SetGameID(id);
  MaybeCreateIcFiles(r, id);
  MaybeCreateDeckFile(r, id);
  std::function<std::unique_ptr<Triforce::DeckReader>()> make = [] {
    return std::make_unique<Triforce::DeckReader>();
  };
  auto dev = make();
  DeckModel model;
  DriveSerial(r, dev, [&](Rng& rr) { return GenDeck(rr, model); }, 400, make, "DECK",
              [&](Rng& rr) {
                const u32 k = rr.Below(100);
                if (k < 4)
                  dev->GetICCardReader()->InsertCard();
                else if (k < 6)
                  dev->GetICCardReader()->EjectCard();
                else if (k < 8)
                {
                  // Let a firmware update time out.
                  const u32 n = 200 + rr.Below(60);
                  for (u32 i = 0; i < n; ++i)
                    dev->Update();
                  RecordTx("DECK.idle", *dev);
                }
                else
                  return false;
                return true;
              });
}

void CaseMag(Rng& r, bool br)
{
  const char* id = br ? "GGGE8P" : (r.Pct(50) ? "GKPJ8P" : "GNJJ8P");
  SetGameID(id);
  MagCard::MagneticCardReader::Settings settings;
  settings.card_path = TriDir();
  settings.card_name = fmt::format("tricard_{}.bin", id);
  settings.insert_card_when_possible = r.Pct(80);
  settings.report_dispenser_empty = r.Pct(20);
  const std::string path = settings.card_path + settings.card_name;
  MaybeCreateMagFile(r, path);
  std::function<std::unique_ptr<MagCard::MagneticCardReader>()> make =
      [&]() -> std::unique_ptr<MagCard::MagneticCardReader> {
    if (br)
      return std::make_unique<MagCard::C1231BR>(&settings);
    return std::make_unique<MagCard::C1231LR>(&settings);
  };
  auto dev = make();
  DriveSerial(r, dev, GenMag, 600, make, br ? "MAGBR" : "MAGLR", [&](Rng& rr) {
    const u32 k = rr.Below(100);
    if (k < 2)
    {
      settings.insert_card_when_possible = !settings.insert_card_when_possible;
      B(fmt::format("MAG.settings insert={}", settings.insert_card_when_possible));
    }
    else if (k < 3)
    {
      settings.report_dispenser_empty = !settings.report_dispenser_empty;
      B(fmt::format("MAG.settings empty={}", settings.report_dispenser_empty));
    }
    else if (k < 5)
    {
      if (rr.Pct(40))
        fs::remove(path);
      else
        WriteFile(path, rr.Bytes(rr.Pct(80) ? 207 : 69));
      B("FILE.toggle mag");
    }
    else
      return false;
    return true;
  });
  dev.reset();
}

// ================================================================================================
// Cases: EXI baseboard
// ================================================================================================

void CaseExiBaseboard(Rng& r)
{
  static constexpr const char* ids[] = {"GHJJ8P", "GGXJ8P", "GLJJ8P", "GGGE8P"};
  const char* id = ids[r.Below(4)];
  SetGameID(id);
  const std::string path = fmt::format("{}tribackup_{}.bin", TriDir(), id);
  static constexpr std::size_t sizes[] = {0x400, 0x800, 0x3FF, 0x100};
  if (r.Pct(70))
    WriteFile(path, r.Bytes(sizes[r.Below(4)]));

  auto make = [] { return std::make_unique<ExpansionInterface::CEXIBaseboard>(Sys()); };
  auto dev = make();
  for (int op = 0; op < 400; ++op)
  {
    const u32 k = r.Below(100);
    if (k < 3)
    {
      auto buf = SaveState([&](PointerWrap& p) { dev->DoState(p); });
      auto fresh = make();
      LoadState(buf, [&](PointerWrap& p) { fresh->DoState(p); });
      dev = std::move(fresh);
    }
    else if (k < 8)
    {
      B(fmt::format("BB.irq {}", dev->IsInterruptSet()));
    }
    else if (k < 10)
    {
      const int flag = static_cast<int>(r.Pct(50) ? 0x04 : r.Below(0x20));
      ExpansionInterface::GenerateInterrupt(flag);
    }
    else if (k < 16)
    {
      static constexpr u32 addrs[] = {0x80010000, 0x80020100, 0x817FFF00, 0x81800000};
      const u32 addr = addrs[r.Below(4)] + 4 * r.Below(16);
      const u32 size = r.Pct(80) ? r.Below(0x200) : 0x400;
      if (r.Pct(50))
      {
        RamWrite((addr & 0x3FFFFFFF) < RAM_SIZE ? addr : 0x80010000,
                 r.Bytes(std::min<u32>(size, 0x100)));
        dev->DMAWrite(addr, size);
        B(fmt::format("BB.dmawrite {:08x} {:x}", addr, size));
      }
      else
      {
        dev->DMARead(addr, size);
        const u32 phys = addr & 0x3FFFFFFF;
        const u32 n = phys < RAM_SIZE ? std::min(size, RAM_SIZE - phys) : 0;
        B(fmt::format("BB.dmaread {:08x} {:x} ram={:016x}", addr, size,
                      n ? Fnv(RamRead(addr, n)) : 0));
      }
    }
    else if (k < 20)
    {
      dev->SetCS(static_cast<int>(r.Below(2)));
      B("BB.cs");
    }
    else
    {
      // A transaction: chip select, 4 command bytes, then reads or writes.
      dev->SetCS(1);
      static constexpr u8 cmds[] = {0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x05,
                                    0x82, 0x83, 0x86, 0x87, 0xFF, 0xFF, 0x44};
      const u8 c0 = r.Pct(95) ? cmds[r.Below(sizeof(cmds))] : r.Byte();
      u8 c1 = static_cast<u8>(r.Below(8));
      u8 c2 = r.Byte();
      if (c0 == 0x00 && r.Pct(80))
        c1 = 0;
      if (c0 == 0xFF && r.Pct(70))
      {
        c1 = r.Pct(50) ? 0 : 2;
        c2 = c1 == 0 ? 0 : 1;
      }
      const u32 word = (u32{c0} << 24) | (u32{c1} << 16) | (u32{c2} << 8) | r.Byte();
      if (r.Pct(85))
      {
        dev->ImmWrite(word, 4);
      }
      else
      {
        dev->ImmWrite(word, 2);
        dev->ImmWrite(word << 16, 2);
      }
      const u32 more = r.Below(4);
      std::string text;
      for (u32 i = 0; i < more; ++i)
      {
        const u32 size = 1 + r.Below(4);
        if (r.Pct(75))
          text += fmt::format(" r{}={:08x}", size, dev->ImmRead(size));
        else
        {
          const u32 v = r.U32();
          dev->ImmWrite(v, size);
          text += fmt::format(" w{}={:08x}", size, v);
        }
      }
      B(fmt::format("BB.xfer {:08x}{}", word, text));
    }
  }
  dev.reset();
}

// ================================================================================================
// Cases: AGP
// ================================================================================================

// The harness's model of the AGP command state, used only to avoid the old code's undefined
// behaviour (docs/w43-spec.md section 13): unsafe operations are skipped.
struct AgpModel
{
  u32 cmd = 0;
  u32 ret = 0;
  u16 pos = 0;
  bool read_bit = false;
  bool has_eeprom = false;
  u32 ee_size = 0;
  u32 ee_mask = 0;
  u32 end = 0;

  bool WriteSafe(u32 d, u32 size) const
  {
    if (size == 1 && (d & 0xFF000000) == 0)
      return true;
    if (cmd != 0xAE0C0000)
      return true;
    const u32 special = read_bit ? end : end + 64;
    if (pos < end || pos == special)
      return true;
    return pos <= end + 63;
  }

  void Write(u32 d, u32 size)
  {
    if (size == 1 && (d & 0xFF000000) == 0)
      return;
    switch (cmd)
    {
    case 0xAE020000:
    case 0xAE030000:
      ret = 0;
      break;
    case 0xAE040000:
    case 0xAE070000:
      pos = static_cast<u16>((d >> 16) & ee_mask);
      break;
    case 0xAE0C0000:
      if (pos == 1 && pos < end)
        read_bit = ((d >> 16) & 1) != 0;
      ++pos;
      ret = 0;
      break;
    case 0xAE0B0000:
      break;
    case 0xAE000000:
    case 0xAE010000:
    case 0xAE090000:
    case 0xAE0A0000:
      pos = 0;
      cmd = d;
      ret = 0;
      break;
    default:
      cmd = d;
      ret = 0;
      break;
    }
  }

  bool ReadSafe() const
  {
    if (cmd == 0xAE040000)
      return ee_size == 0 || pos < ee_size;
    if (cmd == 0xAE0B0000)
      return ee_size == 0 || pos < 4 || pos <= 67;
    return true;
  }

  void Read(u32 size)
  {
    switch (cmd)
    {
    case 0xAE010000:
      if (ret == 1)
        cmd = 0;
      else
        ret = 1;
      break;
    case 0xAE030000:
      if (size == 1)
        cmd = 0;
      break;
    case 0xAE0B0000:
      ++pos;
      cmd = 0;
      break;
    default:
      cmd = 0;
      break;
    }
  }
};

void CaseAgp(Rng& r, bool big_rom)
{
  const auto slot = r.Pct(50) ? ExpansionInterface::Slot::A : ExpansionInterface::Slot::B;
  const std::string work = fs::current_path().string();
  std::string cart = work + "/cart.gba";
  AgpModel model;
  if (r.Pct(8) && !big_rom)
  {
    cart.clear();  // empty path: no ROM, save file ".sav" in the current directory
  }
  Config::SetCurrent(Config::GetInfoForAGPCartPath(slot), cart);
  B(fmt::format("AGP.slot {} path={}", static_cast<int>(slot), cart));

  const std::string base = cart.empty() ? std::string() : work + "/cart";
  if (!cart.empty())
  {
    if (big_rom)
    {
      WriteFile(cart, r.Bytes(0x1000));
      fs::resize_file(cart, 0x2000000);
    }
    else if (r.Pct(85))
    {
      static constexpr std::size_t sizes[] = {0x400, 0x2000, 0x10000, 0x3000, 0x1F0};
      WriteFile(cart, r.Bytes(sizes[r.Below(5)]));
    }
  }
  const std::string sav = (cart.empty() ? std::string() : base) + ".sav";
  static constexpr int sav_sizes[] = {-1, -1, 0, 512, 512, 8192, 8192, 64, 100};
  const int sav_size = sav_sizes[r.Below(9)];
  if (sav_size >= 0)
    WriteFile(sav, r.Bytes(static_cast<std::size_t>(sav_size)));
  model.has_eeprom = sav_size >= 0;
  model.ee_size = sav_size > 0 ? static_cast<u32>(sav_size) : 0;
  model.ee_mask = model.has_eeprom ? static_cast<u32>(sav_size) - 1 : 0;
  model.end = sav_size == 512 ? 8 : (sav_size == 8192 ? 16 : 0);

  auto make = [&] { return std::make_unique<ExpansionInterface::CEXIAgp>(Sys(), slot); };
  auto dev = make();

  static constexpr u32 cmd_words[] = {0xAE000000, 0xAE010000, 0xAE020000, 0xAE030000,
                                      0xAE040000, 0xAE070000, 0xAE090000, 0xAE0A0000,
                                      0xAE0B0000, 0xAE0C0000, 0xAE0C0000, 0xAE0D0000};
  const int ops = big_rom ? 300 : 1200;
  for (int op = 0; op < ops; ++op)
  {
    const u32 k = r.Below(100);
    if (k < 2)
    {
      auto buf = SaveState([&](PointerWrap& p) { dev->DoState(p); });
      auto fresh = make();
      LoadState(buf, [&](PointerWrap& p) { fresh->DoState(p); });
      dev = std::move(fresh);
      continue;
    }
    if (k < 40)
    {
      u32 d = 0;
      u32 size = 4;
      if (k < 18)
      {
        d = r.Pct(92) ? cmd_words[r.Below(sizeof(cmd_words) / 4)] : r.U32();
      }
      else if (k < 30)
      {
        // An EEPROM bit (bit 16) with a non-zero top byte.
        d = 0x01000000 | (r.Below(2) << 16) | (r.U32() & 0xFFFF);
        size = r.Pct(80) ? 2 : 4;
      }
      else
      {
        d = r.U32();
        size = 1 + r.Below(4);
        if (r.Pct(10))
          d &= 0x00FFFFFF;
      }
      if (!model.WriteSafe(d, size))
        continue;
      model.Write(d, size);
      dev->ImmWrite(d, size);
      B(fmt::format("AGP.w {:08x} {}", d, size));
      continue;
    }
    const u32 size = 1 + r.Below(4);
    if (!model.ReadSafe())
    {
      // Leave the command instead (a write is always safe outside AE0C).
      continue;
    }
    model.Read(size);
    B(fmt::format("AGP.r {} = {:08x}", size, dev->ImmRead(size)));
  }
  dev.reset();
}

// ================================================================================================
// Cases: modem
// ================================================================================================

u32 ModemDesc(bool write, bool modem, u32 kind, u32 size, u32 reg)
{
  if (modem)
    return (write ? 0x40000000u : 0u) | 0x20000000u | ((kind & 0xF) << 24) | ((size & 0xFFFF) << 8);
  return (write ? 0x40000000u : 0u) | ((reg & 0x1F) << 24);
}

std::string AtCommand(Rng& r)
{
  static constexpr const char* lines[] = {"ATZ\r",  "ATD5551234\r", "ATDT1\r", "ATH0\r",
                                          "ATH0\n", "AT&F\r",       "ATX3\r",  "AT\r\n",
                                          "ATZ0",   "\r",           "ATD"};
  return lines[r.Below(sizeof(lines) / sizeof(lines[0]))];
}

void CaseModem(Rng& r)
{
  static constexpr const char* dests[] = {"/tmp/w43-tap", "127.0.0.1:7777", ""};
  Config::SetCurrent(Config::MAIN_MODEM_TAPSERVER_DESTINATION, std::string(dests[r.Below(3)]));
  auto dev = std::make_unique<ExpansionInterface::CEXIModem>(
      Sys(), ExpansionInterface::ModemDeviceType::TAPSERVER);
  B(fmt::format("MODEM.present {}", dev->IsPresent()));
  constexpr u32 src = 0x80100000;
  constexpr u32 dst = 0x80200000;

  for (int op = 0; op < 500; ++op)
  {
    const u32 k = r.Below(100);
    if (k < 5)
    {
      dev->SetCS(static_cast<int>(r.Below(2)));
      B("MODEM.cs");
    }
    else if (k < 10)
    {
      B(fmt::format("MODEM.irq {}", dev->IsInterruptSet()));
    }
    else if (k < 16)
    {
      std::string frame;
      const auto bytes = r.Bytes(1 + r.Below(80));
      frame.assign(bytes.begin(), bytes.end());
      TapDeliver(std::move(frame));
    }
    else if (k < 20)
    {
      dev->SetCS(1);
      dev->ImmWrite(0x00008000, 4);
      B("MODEM.reset");
    }
    else if (k < 25)
    {
      // Random raw traffic.
      const u32 size = 1 + r.Below(4);
      if (r.Pct(50))
      {
        const u32 v = r.U32();
        dev->ImmWrite(v, size);
        B(fmt::format("MODEM.w {:08x} {}", v, size));
      }
      else
      {
        B(fmt::format("MODEM.r {} = {:08x}", size, dev->ImmRead(size)));
      }
    }
    else if (k < 32)
    {
      // Register write.
      static constexpr u32 regs[] = {0x01, 0x02, 0x10, 0x11, 0x0E, 0x03, 0x1E, 0x1F};
      const u32 reg = regs[r.Below(8)];
      dev->SetCS(1);
      dev->ImmWrite(ModemDesc(true, false, 0, 0, reg), 4);
      const u32 size = 1 + r.Below(4);
      u32 v = r.U32();
      if (reg == 0x10 && r.Pct(70))
        v = (r.Below(2) << 24) | (r.Byte() << 16);
      dev->ImmWrite(v, size);
      B(fmt::format("MODEM.regw {:02x} {:08x} {}", reg, v, size));
    }
    else if (k < 42)
    {
      // Register read.
      const u32 reg = r.Pct(80) ? r.Below(0x14) : r.Below(0x20);
      dev->SetCS(1);
      dev->ImmWrite(ModemDesc(false, false, 0, 0, reg), 4);
      const u32 size = 1 + r.Below(4);
      B(fmt::format("MODEM.regr {:02x} {} = {:08x}", reg, size, dev->ImmRead(size)));
    }
    else if (k < 62)
    {
      // AT command write, immediate or DMA.
      std::string text = AtCommand(r);
      if (r.Pct(30))
        text += AtCommand(r);
      const u32 len = static_cast<u32>(text.size());
      const u32 size = r.Pct(90) ? len : len + r.Below(4);
      dev->SetCS(1);
      dev->ImmWrite(ModemDesc(true, true, r.Pct(95) ? 3 : 5, size, 0), 4);
      if (r.Pct(50))
      {
        RamWrite(src, {reinterpret_cast<const u8*>(text.data()), text.size()});
        const u32 n = r.Pct(90) ? len : len + 1 + r.Below(3);
        dev->DMAWrite(src, n);
        B(fmt::format("MODEM.atdma {} {}", n, text));
      }
      else
      {
        for (u32 i = 0; i < len;)
        {
          const u32 chunk = std::min<u32>(1 + r.Below(4), len - i);
          u32 v = 0;
          for (u32 j = 0; j < chunk; ++j)
            v |= u32{static_cast<u8>(text[i + j])} << (24 - 8 * j);
          dev->ImmWrite(v, chunk);
          i += chunk;
        }
        B(fmt::format("MODEM.atimm {}", text));
      }
    }
    else if (k < 74)
    {
      // AT reply or packet read.
      const bool packet = r.Pct(50);
      const u32 size = 1 + r.Below(packet ? 0x60 : 0x20);
      dev->SetCS(1);
      dev->ImmWrite(ModemDesc(false, true, packet ? 8 : 3, size, 0), 4);
      if (r.Pct(50))
      {
        std::string text;
        for (u32 i = 0; i < size && i < 16; i += 4)
          text += fmt::format(" {:08x}", dev->ImmRead(r.Pct(80) ? 4 : 1 + r.Below(4)));
        B(fmt::format("MODEM.rdimm {} {}{}", packet, size, text));
      }
      else
      {
        const u32 n = r.Pct(85) ? size : size + 1 + r.Below(4);
        RamWrite(dst, std::vector<u8>(0x80, 0xCC));
        dev->DMARead(dst, n);
        B(fmt::format("MODEM.rddma {} {} {} {}", packet, size, n, Hex(RamRead(dst, 0x80))));
      }
    }
    else if (k < 90)
    {
      // Packet write with HDLC-like frames.
      std::vector<u8> data;
      const u32 frames = r.Below(3);
      for (u32 i = 0; i < frames; ++i)
      {
        data.push_back(0x7E);
        Append(data, r.Bytes(r.Below(12)));
        data.push_back(0x7E);
      }
      if (r.Pct(40))
        Append(data, r.Bytes(1 + r.Below(5)));
      if (data.empty())
        data.push_back(0x7E);
      const u32 len = static_cast<u32>(data.size());
      dev->SetCS(1);
      dev->ImmWrite(ModemDesc(true, true, 8, r.Pct(90) ? len : len - 1 + r.Below(3), 0), 4);
      RamWrite(src, data);
      dev->DMAWrite(src, len);
      B(fmt::format("MODEM.pktdma {}", Hex(data)));
    }
    else
    {
      // DMA without a proper descriptor.
      dev->SetCS(1);
      if (r.Pct(50))
        dev->ImmWrite(ModemDesc(r.Pct(50), false, 0, 0, r.Below(0x20)), 4);
      if (r.Pct(50))
        dev->DMAWrite(src, 1 + r.Below(8));
      else
        dev->DMARead(dst, 1 + r.Below(8));
      B("MODEM.baddma");
    }
  }
  dev.reset();
  TapForget();
}

// ================================================================================================
// Cases: media board
// ================================================================================================

constexpr u32 MB_SRC = 0x80300000;
constexpr u32 MB_DST = 0x80400000;
constexpr u32 NCB = 0x1F800200;  // network command buffer
constexpr u32 NB = 0x1FA00000;   // network buffer

struct MB
{
  u32 ka = 0, kb = 0, kc = 0;
  u32 seq = 0;
  Rng* r = nullptr;

  void Keys(u32 a, u32 b, u32 c)
  {
    AMMediaboard::InitKeys(a, b, c);
    ka = a;
    kb = b;
    kc = c;
    B(fmt::format("MB.keys {:08x} {:08x} {:08x}", a, b, c));
  }

  u32 Exec(u32 opcode, u32 offset, u32 address, u32 length)
  {
    u16 seed = 1;
    if (r->Pct(4))
      seed = static_cast<u16>(r->U32());
    else if (r->Pct(1))
      seed = 0;
    const u32 p0 = (u32{seed} << 16) | (r->Byte() << 8) | opcode;
    const u32 p1 = offset >> 2;
    const u32 p2 = r->U32();
    std::array<u32, 3> cmd{p0 ^ ka, p1 ^ kb, p2 ^ kc};
    u32 imm = 0xDEADBEEF;
    const u32 ret = AMMediaboard::ExecuteCommand(cmd, &imm, address, length);
    ka *= seed;
    kb *= seed;
    kc *= seed;
    if (ka == 0 && RamReadBE32(0) != 0)
    {
      ka = RamReadBE32(0);
      kb = RamReadBE32(4);
      kc = RamReadBE32(8);
    }
    std::string ram;
    const u32 phys = address & 0x3FFFFFFF;
    if (phys >= 0x1000 && phys < RAM_SIZE && length != 0)
    {
      const u32 n = std::min(length, RAM_SIZE - phys);
      ram = n <= 64 ? Hex(RamRead(address, n)) : fmt::format("{:016x}", Fnv(RamRead(address, n)));
    }
    B(fmt::format("MB.exec op={:02x} off={:08x} addr={:08x} len={:x} ret={} imm={:08x} "
                  "cmd={:08x},{:08x},{:08x} ram={}",
                  opcode, offset, address, length, ret, imm, cmd[0], cmd[1], cmd[2], ram));
    return ret;
  }

  void WriteGuest(u32 offset, std::span<const u8> data)
  {
    RamWrite(MB_SRC, data);
    Exec(0xAA, offset, MB_SRC, static_cast<u32>(data.size()));
  }

  std::vector<u8> ReadGuest(u32 offset, u32 length)
  {
    RamWrite(MB_DST, std::vector<u8>(length, 0xA5));
    Exec(0xA8, offset, MB_DST, length);
    return RamRead(MB_DST, length);
  }

  // Board command through Execute1 (0x84000040 write).
  std::vector<u8> Execute1(u16 cmd, u32 w9, u32 w10)
  {
    std::vector<u8> d = r->Bytes(0x20);
    PutLE32(d, 0, (u32{cmd} << 16) | (++seq & 0xFFFF));
    PutLE32(d, 4, w9);
    PutLE32(d, 8, w10);
    WriteGuest(0x84000020, d);
    std::vector<u8> trigger = r->Bytes(0x20);
    trigger[0] = r->Pct(90) ? 1 : 2;
    WriteGuest(0x84000040, trigger);
    return ReadGuest(0x84000000, 0x20);
  }

  // Board command through Execute2 (0x88000000 read); params are words 2-7.
  std::vector<u8> Execute2(u16 cmd, const std::array<u32, 6>& params)
  {
    std::vector<u8> d(0x20);
    PutLE32(d, 0, (u32{cmd} << 16) | (++seq & 0xFFFF));
    PutLE32(d, 4, r->U32());
    for (int i = 0; i < 6; ++i)
      PutLE32(d, 8 + 4 * i, params[i]);
    WriteGuest(0x89000200, d);
    RamWrite(MB_DST, std::vector<u8>(0x40, 0x5A));
    Exec(0xA8, 0x88000000, MB_DST, r->Pct(80) ? 0x20 : 0x40);
    return ReadGuest(0x89000000, 0x20);
  }

  // Board command through the DI Execute opcode; params are words 10-15.
  std::vector<u8> ExecuteDI(u16 cmd, const std::array<u32, 6>& params)
  {
    std::vector<u8> d(0x20);
    PutLE32(d, 0, (u32{cmd} << 16) | (++seq & 0xFFFF));
    PutLE32(d, 4, r->U32());
    for (int i = 0; i < 6; ++i)
      PutLE32(d, 8 + 4 * i, params[i]);
    WriteGuest(0x84000020, d);
    Exec(0xAB, 0, 0, 0);
    return ReadGuest(0x84000000, 0x20);
  }
};

void MbSetup(Rng& r, MB& mb)
{
  mb.r = &r;
  if (r.Pct(60))
    WriteFile(TriDir() + "segaboot.gcm", r.Bytes(0x100 + r.Below(0x4000)));
  if (r.Pct(40))
    WriteFile(TriDir() + "trinetcfg.bin", r.Bytes(r.Pct(50) ? 0x80 : 0x20));
  if (r.Pct(30))
    WriteFile(TriDir() + "triextra.bin", r.Bytes(0x20));
  // Keys from RAM (the first ExecuteCommand picks these up when the keys are zero).
  if (r.Pct(40))
  {
    RamWriteBE32(0, r.U32() | 1);
    RamWriteBE32(4, r.U32());
    RamWriteBE32(8, r.U32());
  }
  if (r.Pct(30))
    RamWriteBE32(0x811FFF00, 1);
  if (r.Pct(15))
    RamWriteBE32(0x8006BF70, 0x0A536567);
  AMMediaboard::Init();
  B(fmt::format("MB.init testmenu={} type={} media={}", AMMediaboard::GetTestMenu(),
                AMMediaboard::GetGameType(), AMMediaboard::GetMediaType()));
  AMMediaboard::InitDIMM(FakeVolume(0x01000000));
  if (r.Pct(50))
    mb.Keys(r.U32(), r.U32(), r.U32());
  if (r.Pct(30))
  {
    const bool on = r.Pct(50);
    AMMediaboard::FirmwareMap(on);
    B(fmt::format("MB.firmwaremap {}", on));
  }
}

void MbStateRoundTrip()
{
  auto buf = SaveState([](PointerWrap& p) { AMMediaboard::DoState(p); });
  LoadState(buf, [](PointerWrap& p) { AMMediaboard::DoState(p); });
}

u32 MbLength(Rng& r)
{
  static constexpr u32 lens[] = {0x20, 0x20, 0x80, 0x40, 0x4, 0x100, 0x200};
  return r.Pct(85) ? lens[r.Below(7)] : 1 + r.Below(0x800);
}

void MbRandomRead(Rng& r, MB& mb)
{
  static constexpr u32 offsets[] = {
      0x80000000, 0x80000020, 0x80000040, 0x80000100, 0x80000120, 0x80000140, 0x80000160,
      0x80000180, 0x800001A0, 0x800001C0, 0x80010000, 0x00000000, 0x1FFEFFE0, 0x1F000000,
      0x1F001000, 0x1F7FFF00, 0xFF000000, 0xFF000400, 0xFFFF0000, 0x89010000, 0x89010004,
      0x84000000, 0x89000000, 0x1F900000, 0x84000010, 0x89000100, 0x1F800200, 0x89040200,
      NB,         0x1FD00000, 0x89100000, 0x89180000, 0x891F0000, 0x1FB00000, 0x1EFF8000,
      0x89010800, 0x88000000, 0x60000000, 0x57058000, 0x00002440, 0x00100000, 0x00FFFFE0,
      0x01000000, 0x0000C000};
  const u32 offset = offsets[r.Below(sizeof(offsets) / 4)];
  u32 length = MbLength(r);
  if (offset == 0)
    length = r.Pct(70) ? 0x80 : length;
  u32 address = MB_DST + 0x20 * r.Below(16);
  if (r.Pct(4))
    address = 0x817FFFF0;
  if (r.Pct(3))
    address = 0x81800000;
  RamWrite(MB_DST, std::vector<u8>(0x400, 0x77));
  mb.Exec(0xA8, offset, address, length);
}

void MbRandomWrite(Rng& r, MB& mb, bool* firmware_map)
{
  static constexpr u32 offsets[] = {
      0x00600000, 0x00700000, 0x00400000, 0x00500000, 0x00600000, 0x00000000, 0x1FFEFFE0,
      0x000006A0, 0x00001000, 0x1F000000, 0x1F000800, 0x84800000, 0x84810000, 0xFF000000,
      0xFFFF0000, 0x84000040, 0x84000020, 0x84000000, 0x89000200, 0x1F800200, 0x89040200,
      NB,         0x1EFF8000, 0x89010000, 0x60000000, 0x40000000, 0x00800000};
  const u32 offset = offsets[r.Below(sizeof(offsets) / 4)];
  u32 length = MbLength(r);
  if (offset == 0x00600000 || offset == 0x00700000)
    length = r.Pct(70) ? 0x20 : length;
  std::vector<u8> data = r.Bytes(std::min<u32>(length, 0x800));
  if (!data.empty() && r.Pct(40))
    data[0] = 1;
  RamWrite(MB_SRC, data);
  u32 address = MB_SRC;
  if (r.Pct(3))
    address = 0x81800000;
  mb.Exec(0xAA, offset, address, length);
  (void)firmware_map;
}

void MbBoardCommand(Rng& r, MB& mb)
{
  static constexpr u16 cmds1[] = {0x000, 0x001, 0x100, 0x101, 0x102, 0x103, 0x104, 0x301,
                                  0x301, 0x8001, 0x0222};
  static constexpr u16 cmds2[] = {0x001, 0x101, 0x102, 0x103, 0x104, 0x204, 0x301, 0x301,
                                  0x414, 0x601, 0x700, 0x8123, 0x0555, 0x411};
  static constexpr u16 cmdsdi[] = {0x000, 0x001, 0x100, 0x100, 0x101, 0x102, 0x103, 0x104,
                                   0x204, 0x301, 0x601, 0x605, 0x606, 0x607, 0x608, 0x614,
                                   0x401, 0x8077, 0x0999};
  const u32 path = r.Below(3);
  std::array<u32, 6> params{};
  for (auto& p : params)
    p = r.Pct(50) ? r.U32() : r.Below(16);
  if (r.Pct(60))
    params[0] = MB_DST + 0x100;  // test string pointer / search address
  if (path == 0)
  {
    const u16 cmd = cmds1[r.Below(sizeof(cmds1) / 2)];
    const u32 w10 = r.Pct(70) ? MB_DST + 0x100 : 0;
    B(fmt::format("MB.exec1 {:04x} -> {}", cmd, Hex(mb.Execute1(cmd, r.U32(), w10))));
  }
  else if (path == 1)
  {
    const u16 cmd = cmds2[r.Below(sizeof(cmds2) / 2)];
    if (cmd == 0x301)
      params[1] = r.Pct(70) ? MB_DST + 0x180 : 0;
    B(fmt::format("MB.exec2 {:04x} -> {}", cmd, Hex(mb.Execute2(cmd, params))));
  }
  else
  {
    const u16 cmd = cmdsdi[r.Below(sizeof(cmdsdi) / 2)];
    if (cmd == 0x301)
      params[2] = MB_DST + 0x1C0;
    if (cmd == 0x607)
    {
      params[0] = r.Pct(70) ? NCB : 0x12345678;
    }
    B(fmt::format("MB.execdi {:04x} -> {}", cmd, Hex(mb.ExecuteDI(cmd, params))));
  }
  B(fmt::format("MB.ram1c0 {}", Hex(RamRead(MB_DST + 0x100, 0xC8))));
}

void CaseMbCmd(Rng& r)
{
  static constexpr const char* ids[] = {"GGGE8P", "GKPJ8P", "GLJJ8P", "GFXJ8P", "GHAE8P",
                                        "G00J8P", "GZZE01"};
  SetGameID(ids[r.Below(7)]);
  MB mb;
  bool firmware_map = false;
  MbSetup(r, mb);
  for (int op = 0; op < 400; ++op)
  {
    const u32 k = r.Below(100);
    if (k < 2)
      MbStateRoundTrip();
    else if (k < 4)
    {
      B(fmt::format("MB.events {}", PendingEventCount()));
      FireScheduledEvents();
    }
    else if (k < 9)
      mb.Exec(0x12, r.Pct(50) ? 0 : 0x2440, MB_DST, 0x20);
    else if (k < 35)
      MbRandomRead(r, mb);
    else if (k < 55)
      MbRandomWrite(r, mb, &firmware_map);
    else if (k < 85)
      MbBoardCommand(r, mb);
    else if (k < 88)
    {
      // DI Execute with other offsets/lengths, and unknown opcodes.
      if (r.Pct(50))
        mb.Exec(0xAB, 4 * r.Below(4), MB_DST, r.Below(2) * 0x20);
      else
        mb.Exec(r.Pct(50) ? 0xE0 : r.Byte() & 0x7F, 0, MB_DST, 0x20);
    }
    else if (k < 90)
    {
      const bool on = r.Pct(50);
      AMMediaboard::FirmwareMap(on);
      B(fmt::format("MB.firmwaremap {}", on));
    }
    else if (k < 91)
    {
      AMMediaboard::Shutdown();
      B("MB.shutdown");
      AMMediaboard::Init();
      AMMediaboard::InitDIMM(FakeVolume(0x01000000));
      mb.ka = mb.kb = mb.kc = 0;
      B(fmt::format("MB.reinit testmenu={}", AMMediaboard::GetTestMenu()));
    }
    else if (k < 93)
    {
      mb.Keys(r.Pct(80) ? r.U32() : 0, r.U32(), r.U32());
    }
    else
    {
      // Test mode check: read at 0x2440.
      mb.Exec(0xA8, 0x2440, MB_DST, 0x20);
    }
  }
  AMMediaboard::Shutdown();
}

// ---- Network -------------------------------------------------------------------------------

std::vector<u8> GuestAddr(Rng& r)
{
  static constexpr u8 ips[][4] = {{192, 168, 29, 150}, {192, 168, 29, 151}, {192, 168, 29, 105},
                                  {192, 168, 150, 16}, {10, 0, 0, 1},       {0, 0, 0, 0},
                                  {127, 0, 0, 1},      {192, 168, 13, 7}};
  static constexpr u16 ports[] = {5000, 5003, 5008, 80, 0, 12345, 5010};
  std::vector<u8> a(16);
  a[0] = r.Pct(70) ? 0 : r.Byte();
  a[1] = static_cast<u8>(r.Pct(90) ? 2 : r.Below(30));
  const u16 port = ports[r.Below(7)];
  a[2] = static_cast<u8>(port >> 8);
  a[3] = static_cast<u8>(port);
  std::memcpy(&a[4], ips[r.Below(8)], 4);
  if (r.Pct(20))
    std::ranges::copy(r.Bytes(8), a.begin() + 8);
  return a;
}

struct NetState
{
  std::vector<u32> sockets;

  u32 Pick(Rng& r) const
  {
    if (!sockets.empty() && r.Pct(85))
      return sockets[r.Below(static_cast<u32>(sockets.size()))];
    return r.Pct(50) ? r.Below(8) : 60 + r.Below(10);
  }
};

void MbNetCommand(Rng& r, MB& mb, NetState& net)
{
  // Which path: Execute2 (params at word 2) or DI Execute (params at word 10).
  const bool di = r.Pct(35);
  auto run = [&](u16 cmd, std::array<u32, 6> params) {
    const auto resp = di ? mb.ExecuteDI(cmd, params) : mb.Execute2(cmd, params);
    B(fmt::format("MB.net {} {:04x} -> {}", di ? "di" : "e2", cmd, Hex(resp)));
    return GetLE32(resp, 4);
  };
  const u32 k = r.Below(100);
  if (k < 12)
  {
    const u32 g = run(0x40B, {2, r.Pct(80) ? 1u : 2u, 0, 0, 0, 0});
    if (g < 64)
      net.sockets.push_back(g);
    return;
  }
  if (k < 17)
  {
    const u32 g = net.Pick(r);
    run(0x403, {g, 0, 0, 0, 0, 0});
    std::erase(net.sockets, g);
    return;
  }
  if (k < 27)
  {
    const u32 off = NCB + 0x100;
    mb.WriteGuest(off, GuestAddr(r));
    const u32 len = r.Pct(90) ? 16 : 15;
    run(0x404, {net.Pick(r), r.Pct(95) ? off : 0x12000000, len, 0, 0, 0});
    return;
  }
  if (k < 33)
  {
    if (di)
      return;  // Bind exists only on the Execute2 path
    const u32 off = NCB + 0x120;
    mb.WriteGuest(off, GuestAddr(r));
    run(0x402, {net.Pick(r), r.Pct(95) ? off : 0x12000000, r.Pct(90) ? 16u : 8u, 0, 0, 0});
    return;
  }
  if (k < 37)
  {
    if (di)
      return;
    run(0x408, {net.Pick(r), r.Below(8), 0, 0, 0, 0});
    return;
  }
  if (k < 43)
  {
    if (di)
      return;
    const u32 addr_off = NCB + 0x140;
    const u32 len_off = NCB + 0x160;
    std::vector<u8> len(4);
    PutLE32(len, 0, r.Pct(70) ? 16 : r.Below(20));
    mb.WriteGuest(len_off, len);
    mb.WriteGuest(addr_off, std::vector<u8>(16, 0xEE));
    const u32 mode = r.Below(10);
    const u32 a = mode == 0 ? 0 : addr_off;
    const u32 l = mode == 1 ? 0 : len_off;
    const u32 g = run(0x401, {net.Pick(r), a, l, 0, 0, 0});
    if (g < 64)
      net.sockets.push_back(g);
    B(fmt::format("MB.accept.addr {}", Hex(mb.ReadGuest(addr_off, 0x24))));
    return;
  }
  if (k < 53)
  {
    const u32 off = NB + 0x100 * r.Below(4);
    const u32 len = r.Pct(90) ? 1 + r.Below(0x100) : 0x20000;
    if (r.Pct(50))
    {
      mb.WriteGuest(off, r.Bytes(std::min<u32>(len, 0x100)));
      run(0x40A, {net.Pick(r), off, len, 0, 0, 0});
    }
    else
    {
      run(0x409, {net.Pick(r), r.Pct(95) ? off : 0x12000000, len, 0, 0, 0});
      B(fmt::format("MB.recv.buf {}", Hex(mb.ReadGuest(off, 0x40))));
    }
    return;
  }
  if (k < 63)
  {
    // Select with fd sets and a timeout.
    const u32 rd = NCB + 0x200, wr = NCB + 0x220, ex = NCB + 0x240, tv = NCB + 0x260;
    for (const u32 off : {rd, wr, ex})
    {
      std::vector<u8> set(32);
      for (const u32 g : net.sockets)
      {
        if (r.Pct(60) && g < 256)
          set[g / 8] |= static_cast<u8>(1u << (g % 8));
      }
      if (r.Pct(10))
        set[r.Below(8)] |= static_cast<u8>(1u << r.Below(8));
      mb.WriteGuest(off, set);
    }
    std::vector<u8> t(16);
    t[0] = static_cast<u8>(r.Below(3));
    PutLE32(t, 8, r.Below(2000000));
    mb.WriteGuest(tv, t);
    const u32 nfds = r.Pct(80) ? 64 : r.Below(80);
    run(0x40C, {nfds, r.Pct(85) ? rd : 0, r.Pct(70) ? wr : 0, r.Pct(30) ? ex : 0,
                r.Pct(80) ? tv : (r.Pct(50) ? 0 : 0x12000000), 0});
    B(fmt::format("MB.select.sets {}", Hex(mb.ReadGuest(rd, 0x60))));
    return;
  }
  if (k < 68)
  {
    const u32 off = NCB + 0x280;
    std::vector<u8> v = r.Bytes(8);
    mb.WriteGuest(off, v);
    run(0x40E, {net.Pick(r), r.Pct(70) ? 1u : 0xFFFFu, r.Pct(50) ? 2u : r.Below(30),
                r.Pct(95) ? off : 0x12000000, r.Pct(90) ? 4u : 0x100000u, 0});
    return;
  }
  if (k < 72)
  {
    if (di)
      return;
    run(0x410, {net.Pick(r), r.Below(100000), r.Below(100000), r.Below(100000), 0, 0});
    return;
  }
  if (k < 76)
  {
    if (di)
      return;
    run(0x411, {net.Pick(r), 0, 0, 0, 0, 0});
    return;
  }
  if (k < 80)
  {
    if (di)
      return;
    static constexpr const char* strs[] = {"192.168.1.2", "10.0.0.255", "999.1.1.1",
                                           "192.168.29.150", "abc"};
    std::string s = strs[r.Below(5)];
    std::vector<u8> d(s.begin(), s.end());
    d.resize(r.Pct(80) ? 16 : d.size());
    mb.WriteGuest(NCB, d);
    run(0x406, {0, 0, 0, 0, 0, 0});
    return;
  }
  if (k < 86)
  {
    static constexpr const char* strs[] = {"192.168.29.150", "192.168.29.151", "10.0.0.7",
                                           "192.168.29.153:1", "bogus", "192.168.150.20"};
    std::string s = strs[r.Below(6)];
    std::vector<u8> d(s.begin(), s.end());
    d.push_back(0);
    const u32 off = NCB + 0x2C0;
    mb.WriteGuest(off, d);
    run(0x415, {r.Pct(95) ? off : 0x12000000, 0, 0, 0, 0, 0});
    return;
  }
  if (k < 90)
  {
    static constexpr u16 misc[] = {0x414, 0x601, 0x700, 0x8402, 0x0444};
    if (di)
      return;
    run(misc[r.Below(5)], {r.U32(), 0, 0, 0, 0, 0});
    return;
  }
  if (k < 93)
  {
    for (const u32 fd : {0u, 1u, 2u, 5u, 63u, 64u, 100u})
      B(fmt::format("MB.dbgsock {} = {}", fd, AMMediaboard::DebuggerGetSocket(fd)));
    return;
  }
  if (k < 95)
  {
    MbStateRoundTrip();
    return;
  }
  // Network buffer reads through the address ranges.
  B(fmt::format("MB.nb {}", Hex(mb.ReadGuest(NB, 0x40))));
}

void CaseMbNet(Rng& r)
{
  SetGameID(r.Pct(50) ? "GKPJ8P" : "GGGE8P");
  if (r.Pct(30))
  {
    Config::SetCurrent(Config::MAIN_TRIFORCE_IP_REDIRECTIONS,
                       std::string("192.168.29.150:5000-5008=127.0.0.1:6000-6008 a,"
                                   "10.0.0.0/24=127.0.0.2:7000 b,bad,"
                                   "192.168.29.151=127.0.0.1:52000-52999 c"));
  }
  MB mb;
  MbSetup(r, mb);
  NetState net;
  for (int op = 0; op < 300; ++op)
    MbNetCommand(r, mb, net);
  AMMediaboard::Shutdown();
}

// ---- Media board functions without a running board -----------------------------------------

std::string RedirPiece(Rng& r)
{
  static constexpr const char* pieces[] = {
      "10.0.0.1=127.0.0.1",
      "10.0.0.0/24=127.0.0.1:5000-5255 subnet",
      "192.168.1.5:80=10.1.1.1:8080 web",
      "192.168.1.5:80-90=10.1.1.1:8080-8090",
      "1.2.3.4-1.2.3.9=5.6.7.8-5.6.7.9 wrap",
      "10.0.0.1=notanip bad",
      "garbage",
      "",
      "1.2.3.4:99999=1.1.1.1",
      "a=b=c",
      "=1.1.1.1",
      "1.1.1.1= x",
      " 1.1.1.1=2.2.2.2",
      "192.168.29.150=127.0.0.1:50000-50999 MarioKart Seat #1 Ephemeral",
  };
  return pieces[r.Below(sizeof(pieces) / sizeof(pieces[0]))];
}

void CaseMbMisc(Rng& r)
{
  // Game type and media type for many IDs (unknown ones raise an alert per call).
  static constexpr const char* ids[] = {
      "GGGE8P", "GHAE8P", "GKPJ8P", "GKZJ8P", "GNJJ8P", "GNLJ8P", "GEJJ8P", "GEYJ8P", "GSBJ8P",
      "G12J8P", "GELJ8P", "GHJJ8P", "GHNJ8P", "GHZJ8P", "GJAJ8P", "GJJJ8P", "GLJJ8P", "GLKJ8P",
      "GLLJ8P", "GGXJ8P", "GFXJ8P", "GJNJ8P", "G00J8P", "GZZE01", "ABC",    "",       "GGGE8PX"};
  for (int i = 0; i < 40; ++i)
  {
    const char* id = ids[r.Below(sizeof(ids) / sizeof(ids[0]))];
    SetGameID(id);
    B(fmt::format("MB.type {} = {} {}", id, AMMediaboard::GetGameType(),
                  AMMediaboard::GetMediaType()));
  }

  for (int i = 0; i < 60; ++i)
  {
    std::string s = RedirPiece(r);
    if (r.Pct(30))
      s += " " + RedirPiece(r);
    const auto parsed = AMMediaboard::ParseIPRedirection(s);
    if (parsed)
    {
      B(fmt::format("MB.parse [{}] = [{}] [{}] [{}]", s, parsed->original, parsed->replacement,
                    parsed->description));
    }
    else
    {
      B(fmt::format("MB.parse [{}] = none", s));
    }
  }

  for (int round = 0; round < 6; ++round)
  {
    if (round > 0)
    {
      std::string list;
      const u32 n = r.Below(6);
      for (u32 i = 0; i < n; ++i)
      {
        if (i != 0)
          list += ",";
        list += RedirPiece(r);
      }
      Config::SetCurrent(Config::MAIN_TRIFORCE_IP_REDIRECTIONS, list);
    }
    const auto redirs = AMMediaboard::GetIPRedirections();
    B(fmt::format("MB.redirs n={}", redirs.size()));
    for (const auto& red : redirs)
    {
      B("MB.redir " + red.ToString());
      for (int j = 0; j < 6; ++j)
      {
        Common::IPv4Port subject{};
        if (r.Pct(60))
          subject = r.Pct(50) ? red.original.first : red.replacement.last;
        else
        {
          for (auto& b : subject.ip_address)
            b = r.Byte();
          subject.port = static_cast<u16>(r.U32());
        }
        if (r.Pct(50))
          subject.port = static_cast<u16>(subject.port + 0x0100 * r.Below(3));
        const auto a = red.Apply(subject);
        const auto v = red.Reverse(subject);
        B(fmt::format("MB.apply {}:{} -> {}:{} rev {}:{}",
                      Common::IPAddressToString(subject.ip_address), subject.port,
                      Common::IPAddressToString(a.ip_address), a.port,
                      Common::IPAddressToString(v.ip_address), v.port));
      }
    }
  }
}

// ================================================================================================
// Case table and runner
// ================================================================================================

struct CaseKind
{
  std::string name;
  std::function<void(Rng&)> run;
};

std::vector<CaseKind> AllKinds()
{
  std::vector<CaseKind> kinds;
  kinds.push_back({"jvs-namco", [](Rng& r) { CaseJvs(r, 0); }});
  kinds.push_back({"jvs-13551", [](Rng& r) { CaseJvs(r, 1); }});
  kinds.push_back({"jvs-13844", [](Rng& r) { CaseJvs(r, 2); }});
  for (const auto& g : GAMES)
    kinds.push_back({g.kind, [&g](Rng& r) { CaseSI(r, g); }});
  kinds.push_back({"wheel-fzero", CaseFzWheel});
  kinds.push_back({"wheel-mkgp", CaseMkWheel});
  kinds.push_back({"touch", CaseTouch});
  kinds.push_back({"iccard", CaseIc});
  kinds.push_back({"deck", CaseDeck});
  kinds.push_back({"magcard-br", [](Rng& r) { CaseMag(r, true); }});
  kinds.push_back({"magcard-lr", [](Rng& r) { CaseMag(r, false); }});
  kinds.push_back({"exi-baseboard", CaseExiBaseboard});
  kinds.push_back({"agp", [](Rng& r) { CaseAgp(r, false); }});
  kinds.push_back({"agp-32m", [](Rng& r) { CaseAgp(r, true); }});
  kinds.push_back({"modem", CaseModem});
  kinds.push_back({"mb-misc", CaseMbMisc});
  kinds.push_back({"mb-cmd", CaseMbCmd});
  kinds.push_back({"mb-net", CaseMbNet});
  return kinds;
}

void HashFiles(const fs::path& root)
{
  std::vector<std::pair<std::string, fs::path>> files;
  for (const auto& entry : fs::recursive_directory_iterator(root))
  {
    if (entry.is_regular_file())
      files.emplace_back(fs::relative(entry.path(), root).generic_string(), entry.path());
  }
  std::ranges::sort(files);
  for (const auto& [rel, path] : files)
  {
    std::ifstream f(path, std::ios::binary);
    std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    B(fmt::format("FILE {} size={} hash={:016x}", rel, data.size(), Fnv(data)));
  }
}

int RunCase(const CaseKind& kind, int index, u64 seed, const std::string& dir, bool echo)
{
  const fs::path case_dir = fs::path(dir) / "case";
  fs::remove_all(case_dir);
  fs::create_directories(case_dir / "user" / "Triforce");
  fs::create_directories(case_dir / "work");
  std::fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0)
  {
    fs::current_path(case_dir / "work");
    File::SetUserPath(D_USER_IDX, (case_dir / "user").string());
    BeginCase(seed, case_dir.string(), echo);
    ResetSockets();
    Rng rng(seed);
    kind.run(rng);
    HashFiles(case_dir);
    B(fmt::format("RAM {:016x}", RamHash()));
    const auto h = EndCase();
    std::printf("%-16s #%-3d seed=%016llx beh=%016llx log=%016llx lines=%llu/%llu\n",
                kind.name.c_str(), index, static_cast<unsigned long long>(seed),
                static_cast<unsigned long long>(h.behaviour),
                static_cast<unsigned long long>(h.logs),
                static_cast<unsigned long long>(h.lines),
                static_cast<unsigned long long>(h.log_lines));
    std::fflush(stdout);
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  fs::remove_all(case_dir);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
  {
    std::printf("%-16s #%-3d seed=%016llx CRASH status=%d\n", kind.name.c_str(), index,
                static_cast<unsigned long long>(seed), status);
    std::fflush(stdout);
    return 1;
  }
  return 0;
}

void Usage()
{
  std::printf("usage: w43h --dir <scratch dir> [--seed N] [--cases N] [--only name[,name]]\n"
              "            [--case K] [--trace] [--list]\n");
}
}  // namespace

int main(int argc, char** argv)
{
  u64 seed = 1;
  int cases = 6;
  int only_case = -1;
  std::string dir;
  std::vector<std::string> only;
  bool trace = false;
  bool list = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc)
      {
        Usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--seed")
      seed = std::stoull(next());
    else if (a == "--cases")
      cases = std::stoi(next());
    else if (a == "--case")
      only_case = std::stoi(next());
    else if (a == "--dir")
      dir = next();
    else if (a == "--only")
    {
      std::string s = next();
      std::size_t start = 0;
      while (start <= s.size())
      {
        const auto comma = s.find(',', start);
        only.push_back(s.substr(start, comma == std::string::npos ? std::string::npos :
                                                                    comma - start));
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    }
    else if (a == "--trace")
      trace = true;
    else if (a == "--list")
      list = true;
    else
    {
      Usage();
      return 2;
    }
  }

  const auto kinds = AllKinds();
  if (list)
  {
    for (const auto& k : kinds)
      std::printf("%s\n", k.name.c_str());
    return 0;
  }
  if (dir.empty())
  {
    Usage();
    return 2;
  }
  fs::create_directories(dir);
  dir = fs::canonical(dir).string();

  Config::Init();

  int crashes = 0;
  int total = 0;
  for (const auto& kind : kinds)
  {
    if (!only.empty() && std::ranges::find(only, kind.name) == only.end())
      continue;
    for (int i = 0; i < cases; ++i)
    {
      if (only_case >= 0 && i != only_case)
        continue;
      const u64 case_seed = Mix(seed * 0x9E3779B97F4A7C15ULL ^ FnvStr(kind.name) ^ u64(i));
      crashes += RunCase(kind, i, case_seed, dir, trace);
      ++total;
    }
  }
  std::printf("cases=%d crashes=%d\n", total, crashes);
  return crashes == 0 ? 0 : 1;
}
