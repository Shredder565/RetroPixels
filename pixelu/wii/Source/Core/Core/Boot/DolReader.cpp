// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Boot/DolReader.h"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Common/Align.h"
#include "Common/BitUtils.h"
#include "Common/Crypto/AES.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"
#include "Core/Boot/AncastTypes.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/IOSC.h"
#include "Core/System.h"

namespace
{
// `mfspr rX, HID4`. HID4 only exists on Broadway, so code touching it is
// taken as the sign of a Wii executable. The mask ignores the target register.
constexpr u32 MFSPR_HID4 = 0x7c13fba6;
constexpr u32 MFSPR_HID4_MASK = 0xfc1fffff;

// Loaders round every section up to this size and copy the padding too.
constexpr u64 SECTION_ALIGNMENT = 32;

bool ContainsHID4Access(std::span<const u8> code)
{
  for (size_t at = 0; at + 4 <= code.size(); at += 4)
  {
    if ((Common::swap32(code.data() + at) & MFSPR_HID4_MASK) == MFSPR_HID4)
      return true;
  }
  return false;
}

// The Espresso boot ROM decrypts vWii PPC ancast bodies with keys from the Espresso's own OTP.
// That OTP is not part of a Wii U otp.bin dump (which is the Latte's; WiiUBrew lists no Espresso
// key in it), and RetroPixels has no keys of its own, so they can only come from wii_keys.txt.
std::optional<std::vector<u8>> LoadAncastKey(u32 console_type)
{
  const char* const name = console_type == ANCAST_CONSOLE_TYPE_DEV ? "espresso_ancast_key_dev" :
                                                                     "espresso_ancast_key";
  return IOS::HLE::IOSC::LoadOwnerKey(name, Common::AES::Context::KEY_SIZE);
}
}  // namespace

DolReader::~DolReader() = default;

// All three constructors only differ in how the base class obtains the bytes.
DolReader::DolReader(const std::string& filename) : BootExecutableReader(filename)
{
  m_is_valid = Initialize(m_bytes);
}

DolReader::DolReader(std::vector<u8> buffer) : BootExecutableReader(std::move(buffer))
{
  m_is_valid = Initialize(m_bytes);
}

DolReader::DolReader(File::IOFile file) : BootExecutableReader(std::move(file))
{
  m_is_valid = Initialize(m_bytes);
}

bool DolReader::Initialize(std::span<const u8> buffer)
{
  if (buffer.size() < sizeof(DolHeader) || buffer.size() > UINT32_MAX)
    return false;

  // The header is nothing but big-endian words.
  std::array<u32, sizeof(DolHeader) / sizeof(u32)> words;
  for (size_t i = 0; i < words.size(); ++i)
    words[i] = Common::swap32(buffer.data() + i * sizeof(u32));
  std::memcpy(&m_dolheader, words.data(), sizeof(DolHeader));

  m_is_wii = false;

  // Registers one header entry as a section. Returns false when its aligned
  // extent runs past the end of the file. The rounded-up size is what real
  // loaders read, so a file that is too short for it can't be booted.
  const auto add_section = [&](const char* kind, int index, u32 offset, u32 address,
                               u32 size) -> bool {
    const u64 padded = Common::AlignUp(static_cast<u64>(size), SECTION_ALIGNMENT);
    if (padded != size)
    {
      WARN_LOG_FMT(BOOT,
                   "DolReader: {} section {} size is not aligned to 32 bytes and can cause "
                   "issues when loaded",
                   kind, index);
    }
    if (offset + padded > buffer.size())
      return false;

    m_sections.push_back({address, size, buffer.subspan(offset, static_cast<size_t>(padded))});
    return true;
  };

  for (int i = 0; i < DOL_NUM_TEXT; ++i)
  {
    const u32 size = m_dolheader.m_text_size[i];
    if (size == 0)
      continue;
    if (!add_section("Text", i, m_dolheader.m_text_offset[i], m_dolheader.m_text_address[i], size))
      return false;
    if (!m_is_wii && ContainsHID4Access(m_sections.back().m_data))
      m_is_wii = true;
  }

  for (int i = 0; i < DOL_NUM_DATA; ++i)
  {
    const u32 size = m_dolheader.m_data_size[i];
    if (size == 0)
      continue;
    if (!add_section("Data", i, m_dolheader.m_data_offset[i], m_dolheader.m_data_address[i], size))
      return false;

    // A vWii ancast image, if present, is always the first data section and
    // is linked at the fixed address the Espresso boot ROM expects.
    const LoadableSection& added = m_sections.back();
    const bool ancast = i == 0 && added.m_header_section_size > sizeof(EspressoAncastHeader) &&
                        added.m_address == ESPRESSO_ANCAST_LOCATION_VIRT &&
                        Common::swap32(added.m_data.data()) == ANCAST_MAGIC;
    if (ancast)
      m_ancast_index = m_sections.size() - 1;
  }

  return true;
}

bool DolReader::LoadIntoMemory(Core::System& system, bool only_in_mem1) const
{
  if (!m_is_valid)
    return false;

  if (IsAncast())
    return LoadAncastIntoMemory(system);

  auto& memory = system.GetMemory();
  const u64 mem1_end = memory.GetRamSizeReal();
  for (const LoadableSection& section : m_sections)
  {
    const u64 section_end = u64{section.m_address} + section.m_data.size();
    if (only_in_mem1 && section_end >= mem1_end)
      continue;
    memory.CopyToEmu(section.m_address, section.m_data.data(), section.m_data.size());
  }
  return true;
}

// Does what the Espresso boot ROM would: validate the ancast header, check
// the body hash, decrypt the body and place header plus body in memory.
bool DolReader::LoadAncastIntoMemory(Core::System& system) const
{
  const LoadableSection& section = m_sections[m_ancast_index.value()];
  const u32 load_address = m_dolheader.m_data_address[0];
  const EspressoAncastHeader header =
      Common::BitCastPtr<EspressoAncastHeader>(section.m_data.data());

  const u32 block_size = Common::swap32(header.header_block.header_block_size);
  if (block_size != sizeof(AncastHeaderBlock))
  {
    ERROR_LOG_FMT(BOOT, "Ancast: Invalid header block size: 0x{:x}", block_size);
    return false;
  }

  // PPC images use signature type 1.
  const u32 signature_type = Common::swap32(header.signature_block.signature_type);
  if (signature_type != 0x01)
  {
    ERROR_LOG_FMT(BOOT, "Ancast: Invalid signature type: 0x{:x}", signature_type);
    return false;
  }

  const u32 image_type = Common::swap32(header.info_block.image_type);
  if (image_type != ANCAST_IMAGE_TYPE_ESPRESSO_WII)
  {
    ERROR_LOG_FMT(BOOT, "Ancast: Invalid image type: 0x{:x}", image_type);
    return false;
  }

  const u32 body_size = Common::swap32(header.info_block.body_size);
  if (sizeof(EspressoAncastHeader) + body_size > section.m_header_section_size)
  {
    ERROR_LOG_FMT(BOOT, "Ancast: Invalid body size: 0x{:x}", body_size);
    return false;
  }

  const u8* const body = section.m_data.data() + sizeof(EspressoAncastHeader);
  if (Common::SHA1::CalculateDigest(body, body_size) != header.info_block.body_hash)
  {
    ERROR_LOG_FMT(BOOT, "Ancast: Body hash mismatch");
    return false;
  }

  const u32 console_type = Common::swap32(header.info_block.console_type);
  if (console_type != ANCAST_CONSOLE_TYPE_DEV && console_type != ANCAST_CONSOLE_TYPE_RETAIL)
  {
    ERROR_LOG_FMT(BOOT, "Ancast: Invalid console type: 0x{:x}", console_type);
    return false;
  }

  const std::optional<std::vector<u8>> key = LoadAncastKey(console_type);
  const std::optional<std::vector<u8>> iv =
      IOS::HLE::IOSC::LoadOwnerKey("espresso_ancast_iv", Common::AES::Context::BLOCK_SIZE);
  if (!key || !iv)
  {
    ERROR_LOG_FMT(BOOT, "Ancast: no Espresso ancast key and IV in wii_keys.txt");
    PanicAlertFmtT("This is a vWii ancast image. It is encrypted with keys from the Wii U's "
                   "Espresso processor, which RetroPixels doesn't include and a Wii U otp.bin "
                   "doesn't contain, so it can't be started.\n\nIf you have them from your own "
                   "console, add espresso_ancast_key and espresso_ancast_iv to wii_keys.txt in "
                   "{0}",
                   File::GetUserPath(D_WIIROOT_IDX));
    return false;
  }

  std::vector<u8> plain(body_size);
  const std::unique_ptr<Common::AES::Context> aes = Common::AES::CreateContextDecrypt(key->data());
  if (!aes->Crypt(iv->data(), body, plain.data(), body_size))
    return false;

  auto& memory = system.GetMemory();
  memory.CopyToEmu(load_address, &header, sizeof(EspressoAncastHeader));
  memory.CopyToEmu(load_address + sizeof(EspressoAncastHeader), plain.data(), body_size);
  return true;
}
