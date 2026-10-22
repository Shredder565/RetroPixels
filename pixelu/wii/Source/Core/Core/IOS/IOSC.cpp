// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/IOSC.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <mbedtls/md.h>
#include <mbedtls/rsa.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Crypto/ec.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Common/Swap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/Formats.h"

namespace
{
// Layout of keys.bin, the 1 KiB OTP/SEEPROM dump written by BootMii (see WiiBrew, "keys.bin").
// Offsets are given for each field; multi-byte values are big endian.
#pragma pack(push, 1)
struct KeysBin
{
  std::array<char, 0x100> creator;     // 0x000
  std::array<u8, 0x14> boot1_hash;     // 0x100
  std::array<u8, 0x10> common_key;     // 0x114
  u32 ng_id;                           // 0x124
  std::array<u8, 0x1e> ng_priv;        // 0x128
  std::array<u8, 0x02> unused_146;     // 0x146 (overlaps the NAND HMAC key below)
  std::array<u8, 0x10> unused_148;     // 0x148
  std::array<u8, 0x10> nand_key;       // 0x158
  std::array<u8, 0x10> backup_key;     // 0x168
  u32 unknown_178;                     // 0x178
  u32 unknown_17c;                     // 0x17c
  std::array<u8, 0x80> unused_180;     // 0x180
  u32 ms_id;                           // 0x200
  u32 ca_id;                           // 0x204
  u32 ng_key_id;                       // 0x208
  Common::ec::Signature ng_sig;        // 0x20c
  struct Counter
  {
    u8 boot2version;
    u8 unknown1;
    u8 unknown2;
    u8 pad;
    u32 update_tag;
    u16 checksum;
  };
  std::array<Counter, 2> counters;     // 0x248
  std::array<u8, 0x18> unused_25c;     // 0x25c
  std::array<u8, 0x10> korean_key;     // 0x274
  std::array<u8, 0x74> unused_284;     // 0x284
  std::array<u16, 2> prng_seed;        // 0x2f8
  std::array<u8, 0x04> unused_2fc;     // 0x2fc
  std::array<u8, 0x100> unused_300;    // 0x300
};
static_assert(sizeof(KeysBin) == 0x400, "keys.bin is 1 KiB");
#pragma pack(pop)

// The NAND HMAC key shares bytes with the end of the NG private key region.
constexpr size_t KEYS_BIN_NAND_HMAC_OFFSET = 0x144;
constexpr size_t KEYS_BIN_NAND_HMAC_SIZE = 0x14;

// keys.bin holds the console's 128-byte OTP at 0x100 and its SEEPROM from 0x200 on.
constexpr size_t KEYS_BIN_OTP_OFFSET = 0x100;
constexpr size_t KEYS_BIN_SEEPROM_OFFSET = 0x200;
constexpr size_t WII_OTP_SIZE = 0x80;

// A Wii U OTP dump (WiiUBrew, "Hardware/eFuse") is eight banks of 0x80 bytes. Bank 0 is the
// vWii's own Wii OTP, laid out like a Wii's. Bank 6 starts with the vWii's MS ID, CA ID, NG key ID
// and NG signature (the same fields, in the same order, as the start of a Wii's SEEPROM) and holds
// the Korean common key at 0x348.
constexpr size_t WIIU_OTP_SIZE = 0x400;
constexpr size_t WIIU_OTP_WII_CERT_OFFSET = 0x300;
constexpr size_t WIIU_OTP_WII_CERT_SIZE = 0x48;
constexpr size_t WIIU_OTP_KOREAN_KEY_OFFSET = 0x348;

// CA and MS numbers of retail and development consoles.
constexpr u32 RETAIL_CA_ID = 1;
constexpr u32 RETAIL_MS_ID = 2;
constexpr u32 DEV_CA_ID = 2;
constexpr u32 DEV_MS_ID = 3;

// The owner's key files, all in the Wii NAND root.
constexpr const char* KEYS_BIN_NAME = "keys.bin";
constexpr const char* OTP_BIN_NAME = "otp.bin";
constexpr const char* KEYS_TEXT_NAME = "wii_keys.txt";

template <size_t N>
bool IsAllZero(const std::array<u8, N>& bytes)
{
  return std::ranges::all_of(bytes, [](u8 b) { return b == 0; });
}

std::string NandRoot()
{
  return File::GetUserPath(D_WIIROOT_IDX);
}

// A console's keys as read from the owner's dump, always in the keys.bin layout.
struct OwnerConsoleKeys
{
  KeysBin dump{};
  // False for a Wii otp.bin: it has no SEEPROM, so the NG signature, NG key ID, boot2 version
  // and Korean key are unknown (zero), and the CA/MS numbers are those of a retail console.
  bool has_seeprom = false;
  std::string path;
};

std::optional<OwnerConsoleKeys> ReadKeysBin(const std::string& path)
{
  File::IOFile file{path, "rb"};
  if (!file)
    return std::nullopt;

  OwnerConsoleKeys keys;
  keys.path = path;
  keys.has_seeprom = true;
  if (!file.ReadBytes(&keys.dump, sizeof(keys.dump)) || IsAllZero(keys.dump.common_key))
  {
    ERROR_LOG_FMT(IOS, "{} is not a BootMii keys.bin (1 KiB holding a Wii's keys); ignored", path);
    return std::nullopt;
  }
  return keys;
}

std::optional<OwnerConsoleKeys> ReadOtpBin(const std::string& path)
{
  File::IOFile file{path, "rb"};
  if (!file)
    return std::nullopt;

  const u64 size = file.GetSize();
  std::vector<u8> raw;
  if (size == WII_OTP_SIZE || size == WIIU_OTP_SIZE)
    raw.resize(static_cast<size_t>(size));
  if (raw.empty() || !file.ReadBytes(raw.data(), raw.size()))
  {
    ERROR_LOG_FMT(IOS, "{} is not an OTP dump (128 bytes from a Wii or 1 KiB from a Wii U); ignored",
                  path);
    return std::nullopt;
  }

  OwnerConsoleKeys keys;
  keys.path = path;
  u8* const out = reinterpret_cast<u8*>(&keys.dump);
  std::memcpy(out + KEYS_BIN_OTP_OFFSET, raw.data(), WII_OTP_SIZE);
  if (raw.size() == WIIU_OTP_SIZE)
  {
    std::memcpy(out + KEYS_BIN_SEEPROM_OFFSET, raw.data() + WIIU_OTP_WII_CERT_OFFSET,
                WIIU_OTP_WII_CERT_SIZE);
    std::memcpy(keys.dump.korean_key.data(), raw.data() + WIIU_OTP_KOREAN_KEY_OFFSET,
                keys.dump.korean_key.size());
    keys.has_seeprom = true;
  }
  else
  {
    keys.dump.ms_id = Common::swap32(RETAIL_MS_ID);
    keys.dump.ca_id = Common::swap32(RETAIL_CA_ID);
  }

  if (IsAllZero(keys.dump.common_key))
  {
    ERROR_LOG_FMT(IOS, "{} holds no common key (a blank or damaged OTP dump); ignored", path);
    return std::nullopt;
  }
  return keys;
}

// keys.bin first (it is complete), then otp.bin.
std::optional<OwnerConsoleKeys> ReadOwnerConsoleKeys()
{
  const std::string root = NandRoot();
  if (auto keys = ReadKeysBin(root + KEYS_BIN_NAME))
    return keys;
  return ReadOtpBin(root + OTP_BIN_NAME);
}

IOS::HLE::IOSC::ConsoleType ConsoleTypeOfKeys(const KeysBin& dump)
{
  return Common::swap32(dump.ca_id) == DEV_CA_ID ? IOS::HLE::IOSC::ConsoleType::RVT :
                                                    IOS::HLE::IOSC::ConsoleType::Retail;
}

std::string_view Trim(std::string_view text)
{
  const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && is_space(text.front()))
    text.remove_prefix(1);
  while (!text.empty() && is_space(text.back()))
    text.remove_suffix(1);
  return text;
}

int HexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// wii_keys.txt: one "name = hex bytes" per line (":" works too, spaces between the digits are
// allowed, "#" and ";" start a comment). Names are not case sensitive.
std::optional<std::vector<u8>> ParseKeyEntry(std::string_view text, std::string_view name,
                                             const std::string& path)
{
  while (!text.empty())
  {
    const size_t line_end = std::min(text.find('\n'), text.size());
    std::string_view line = text.substr(0, line_end);
    text.remove_prefix(std::min(line_end + 1, text.size()));

    line = line.substr(0, std::min(line.find_first_of("#;"), line.size()));
    const size_t separator = line.find_first_of("=:");
    if (separator == std::string_view::npos)
      continue;
    const std::string_view entry_name = Trim(line.substr(0, separator));
    const bool same_name =
        entry_name.size() == name.size() &&
        std::equal(entry_name.begin(), entry_name.end(), name.begin(), [](char a, char b) {
          return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
        });
    if (!same_name)
      continue;

    std::vector<u8> bytes;
    int high = -1;
    for (const char c : line.substr(separator + 1))
    {
      if (c == ' ' || c == '\t' || c == '\r')
        continue;
      const int digit = HexDigit(c);
      if (digit < 0)
      {
        ERROR_LOG_FMT(IOS, "{}: \"{}\" is not written in hex digits; ignored", path, name);
        return std::nullopt;
      }
      if (high < 0)
      {
        high = digit;
      }
      else
      {
        bytes.push_back(static_cast<u8>(high << 4 | digit));
        high = -1;
      }
    }
    if (high >= 0 || bytes.empty())
    {
      ERROR_LOG_FMT(IOS, "{}: \"{}\" has an incomplete hex value; ignored", path, name);
      return std::nullopt;
    }
    return bytes;
  }
  return std::nullopt;
}
}  // namespace

namespace IOS::HLE
{
namespace
{
constexpr size_t AES128_KEY_SIZE = 0x10;
constexpr u32 RSA_EXPONENT_65537_BE = 0x01000100;  // 0x00010001 as stored (big endian)

// Bytes of key material an object of the given kind holds (0 = no fixed size / no key).
size_t KeySize(IOSC::ObjectType type, IOSC::ObjectSubType subtype)
{
  using ST = IOSC::ObjectSubType;
  if (type == IOSC::TYPE_SECRET_KEY)
  {
    switch (subtype)
    {
    case ST::AES128:
      return 16;
    case ST::MAC:
      return 20;
    case ST::ECC233:
      return 30;
    default:
      return 0;
    }
  }
  if (type == IOSC::TYPE_PUBLIC_KEY)
  {
    switch (subtype)
    {
    case ST::RSA2048:
      return 256;
    case ST::RSA4096:
      return 512;
    case ST::ECC233:
      return 60;
    default:
      return 0;
    }
  }
  return 0;
}

u32 OwnerBit(u32 pid)
{
  return 1u << pid;
}

// Builds an unsigned ECC certificate for the key derived from `private_key`.
CertECC MakeEccCertificate(const std::string& issuer, const std::string& name,
                           const u8* private_key, u32 key_id)
{
  CertECC cert{};
  cert.signature.type = static_cast<SignatureType>(Common::swap32(u32(SignatureType::ECC)));
  const size_t issuer_len = std::min(issuer.size(), sizeof(cert.signature.issuer) - 1);
  std::memcpy(cert.signature.issuer, issuer.data(), issuer_len);
  cert.header.public_key_type = static_cast<PublicKeyType>(Common::swap32(u32(PublicKeyType::ECC)));
  const size_t name_len = std::min(name.size(), sizeof(cert.header.name) - 1);
  std::memcpy(cert.header.name, name.data(), name_len);
  cert.header.id = Common::swap32(key_id);
  cert.public_key = Common::ec::PrivToPub(private_key);
  return cert;
}
}  // namespace

IOSC::KeyEntry::KeyEntry() = default;

IOSC::KeyEntry::KeyEntry(ObjectType type_, ObjectSubType subtype_, std::vector<u8>&& data_,
                         u32 misc_data_, u32 owner_mask_)
    : in_use(true), type(type_), subtype(subtype_), data(std::move(data_)), misc_data(misc_data_),
      owner_mask(owner_mask_)
{
}

IOSC::KeyEntry::KeyEntry(ObjectType type_, ObjectSubType subtype_, std::vector<u8>&& data_,
                         u32 owner_mask_)
    : KeyEntry(type_, subtype_, std::move(data_), 0, owner_mask_)
{
}

void IOSC::KeyEntry::DoState(PointerWrap& p)
{
  p.Do(in_use);
  p.Do(type);
  p.Do(subtype);
  p.Do(data);
  p.Do(misc_data);
  p.Do(owner_mask);
}

IOSC::IOSC(ConsoleType console_type) : m_console_type(console_type)
{
  LoadDefaultEntries();
  // The keys come from the owner's own console dump; without one the key objects stay empty.
  LoadEntries();
}

IOSC::~IOSC() = default;

// --------------------------------------------------------------------------------------------
// Table management

IOSC::KeyEntries::iterator IOSC::FindFreeEntry()
{
  auto it = m_key_entries.begin();
  while (it != m_key_entries.end() && it->in_use)
    ++it;
  return it;
}

IOSC::KeyEntry* IOSC::FindEntry(Handle handle)
{
  if (handle >= m_key_entries.size())
    return nullptr;
  return &m_key_entries[handle];
}

const IOSC::KeyEntry* IOSC::FindEntry(Handle handle, SearchMode mode) const
{
  if (handle == HANDLE_ROOT_KEY && mode == SearchMode::IncludeRootKey)
    return &m_root_key_entry;
  if (handle >= m_key_entries.size())
    return nullptr;
  return &m_key_entries[handle];
}

IOSC::Handle IOSC::GetHandleFromIterator(IOSC::KeyEntries::iterator iterator) const
{
  ASSERT(iterator != m_key_entries.end());
  return static_cast<Handle>(std::distance(m_key_entries.cbegin(), KeyEntries::const_iterator{iterator}));
}

bool IOSC::IsDefaultHandle(Handle handle) const
{
  return handle == HANDLE_ROOT_KEY || handle <= HANDLE_NEW_COMMON_KEY;
}

bool IOSC::HasOwnership(Handle handle, u32 pid) const
{
  // Everyone may use the root key.
  if (handle == HANDLE_ROOT_KEY)
    return true;
  u32 owners;
  if (GetOwnership(handle, &owners) != IPC_SUCCESS)
    return false;
  return (owners & OwnerBit(pid)) != 0;
}

ReturnCode IOSC::GetOwnership(Handle handle, u32* owner) const
{
  const KeyEntry* entry = FindEntry(handle);
  if (entry == nullptr || !entry->in_use)
    return IOSC_EINVAL;
  *owner = entry->owner_mask;
  return IPC_SUCCESS;
}

ReturnCode IOSC::SetOwnership(Handle handle, u32 new_owner, u32 pid)
{
  if (!HasOwnership(handle, pid))
    return IOSC_EACCES;

  KeyEntry* entry = FindEntry(handle);
  if (entry == nullptr)
    return IOSC_EINVAL;

  // Only an object owned by the caller alone may be handed on. The caller stays an owner;
  // bits 0-2 of the new mask cannot be granted this way.
  const u32 caller = OwnerBit(pid);
  if ((entry->owner_mask & ~caller) != 0)
    return IOSC_EACCES;
  entry->owner_mask = (new_owner & ~7u) | caller;
  return IPC_SUCCESS;
}

ReturnCode IOSC::CreateObject(Handle* handle, ObjectType type, ObjectSubType subtype, u32 pid)
{
  const auto slot = FindFreeEntry();
  if (slot == m_key_entries.end())
    return IOSC_FAIL_ALLOC;

  // The slot's previous key bytes and misc word are left as they were.
  slot->in_use = true;
  slot->type = type;
  slot->subtype = subtype;
  slot->owner_mask = OwnerBit(pid);
  *handle = GetHandleFromIterator(slot);
  return IPC_SUCCESS;
}

ReturnCode IOSC::DeleteObject(Handle handle, u32 pid)
{
  if (IsDefaultHandle(handle) || !HasOwnership(handle, pid))
    return IOSC_EACCES;

  KeyEntry* entry = FindEntry(handle);
  if (entry == nullptr)
    return IOSC_EINVAL;
  entry->in_use = false;
  entry->data.clear();
  return IPC_SUCCESS;
}

// --------------------------------------------------------------------------------------------
// Key import

ReturnCode IOSC::ImportSecretKey(Handle dest_handle, Handle decrypt_handle, u8* iv,
                                 const u8* encrypted_key, u32 pid)
{
  std::array<u8, AES128_KEY_SIZE> plain;
  const ReturnCode result =
      Decrypt(decrypt_handle, iv, encrypted_key, plain.size(), plain.data(), pid);
  if (result != IPC_SUCCESS)
    return result;
  return ImportSecretKey(dest_handle, plain.data(), pid);
}

ReturnCode IOSC::ImportSecretKey(Handle dest_handle, const u8* decrypted_key, u32 pid)
{
  if (!HasOwnership(dest_handle, pid) || IsDefaultHandle(dest_handle))
    return IOSC_EACCES;

  KeyEntry* dest = FindEntry(dest_handle);
  if (dest == nullptr)
    return IOSC_EINVAL;

  // Only AES keys can be imported so far.
  if (dest->type != TYPE_SECRET_KEY || dest->subtype != ObjectSubType::AES128)
    return IOSC_INVALID_OBJTYPE;

  dest->data.assign(decrypted_key, decrypted_key + AES128_KEY_SIZE);
  return IPC_SUCCESS;
}

ReturnCode IOSC::ImportPublicKey(Handle dest_handle, const u8* public_key,
                                 const u8* public_key_exponent, u32 pid)
{
  if (!HasOwnership(dest_handle, pid) || IsDefaultHandle(dest_handle))
    return IOSC_EACCES;

  KeyEntry* dest = FindEntry(dest_handle);
  if (dest == nullptr)
    return IOSC_EINVAL;
  if (dest->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  const size_t length = KeySize(dest->type, dest->subtype);
  if (length == 0)
    return IOSC_INVALID_OBJTYPE;
  dest->data.assign(public_key, public_key + length);

  // RSA keys keep their (big endian) exponent in the misc word.
  const bool is_rsa =
      dest->subtype == ObjectSubType::RSA2048 || dest->subtype == ObjectSubType::RSA4096;
  if (is_rsa)
  {
    ASSERT(public_key_exponent);
    std::memcpy(&dest->misc_data, public_key_exponent, sizeof(dest->misc_data));
  }
  return IPC_SUCCESS;
}

ReturnCode IOSC::ComputeSharedKey(Handle dest_handle, Handle private_handle, Handle public_handle,
                                  u32 pid)
{
  const bool owns_all = HasOwnership(dest_handle, pid) && HasOwnership(private_handle, pid) &&
                        HasOwnership(public_handle, pid);
  if (!owns_all || IsDefaultHandle(dest_handle))
    return IOSC_EACCES;

  KeyEntry* dest = FindEntry(dest_handle);
  const KeyEntry* mine = FindEntry(private_handle);
  const KeyEntry* theirs = FindEntry(public_handle);
  if (dest == nullptr || mine == nullptr || theirs == nullptr)
    return IOSC_EINVAL;

  const bool kinds_ok = dest->type == TYPE_SECRET_KEY && dest->subtype == ObjectSubType::AES128 &&
                        mine->type == TYPE_SECRET_KEY && mine->subtype == ObjectSubType::ECC233 &&
                        theirs->type == TYPE_PUBLIC_KEY && theirs->subtype == ObjectSubType::ECC233;
  if (!kinds_ok)
    return IOSC_INVALID_OBJTYPE;

  // The AES key is the first 16 bytes of SHA-1 over the x coordinate of the shared point.
  const std::array<u8, 0x3c> point =
      Common::ec::ComputeSharedSecret(mine->data.data(), theirs->data.data());
  const auto digest = Common::SHA1::CalculateDigest(point.data(), point.size() / 2);
  dest->data.assign(digest.begin(), digest.begin() + AES128_KEY_SIZE);
  return IPC_SUCCESS;
}

// --------------------------------------------------------------------------------------------
// Using keys

ReturnCode IOSC::DecryptEncrypt(Common::AES::Mode mode, Handle key_handle, u8* iv, const u8* input,
                                size_t size, u8* output, u32 pid) const
{
  if (!HasOwnership(key_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* entry = FindEntry(key_handle);
  if (entry == nullptr)
    return IOSC_EINVAL;
  if (entry->type != TYPE_SECRET_KEY || entry->subtype != ObjectSubType::AES128)
    return IOSC_INVALID_OBJTYPE;
  // An AES object that never had a key imported.
  if (entry->data.size() != AES128_KEY_SIZE)
    return IOSC_FAIL_INTERNAL;

  const u8* key = entry->data.data();
  const auto cipher = mode == Common::AES::Mode::Encrypt ? Common::AES::CreateContextEncrypt(key) :
                                                           Common::AES::CreateContextDecrypt(key);
  cipher->Crypt(iv, iv, input, output, size);
  return IPC_SUCCESS;
}

// Both directions share one implementation; only the AES mode differs.
ReturnCode IOSC::Decrypt(Handle key_handle, u8* iv, const u8* input, size_t size, u8* output,
                         u32 pid) const
{
  constexpr auto mode = Common::AES::Mode::Decrypt;
  return DecryptEncrypt(mode, key_handle, iv, input, size, output, pid);
}

ReturnCode IOSC::Encrypt(Handle key_handle, u8* iv, const u8* input, size_t size, u8* output,
                         u32 pid) const
{
  constexpr auto mode = Common::AES::Mode::Encrypt;
  return DecryptEncrypt(mode, key_handle, iv, input, size, output, pid);
}

ReturnCode IOSC::VerifyPublicKeySign(const std::array<u8, 20>& sha1, Handle signer_handle,
                                     const std::vector<u8>& signature, u32 pid) const
{
  if (!HasOwnership(signer_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* signer = FindEntry(signer_handle, SearchMode::IncludeRootKey);
  if (signer == nullptr)
    return IOSC_EINVAL;
  // Without the owner's root key nothing can be checked against it.
  if (signer_handle == HANDLE_ROOT_KEY && signer->data.empty())
    return IOSC_FAIL_CHECKVALUE;
  // Key pairs are not supported, only public keys.
  if (signer->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  if (signer->subtype == ObjectSubType::ECC233)
  {
    ASSERT(signer->data.size() == sizeof(CertECC::public_key));
    const bool valid =
        Common::ec::VerifySignature(signer->data.data(), signature.data(), sha1.data());
    return valid ? IPC_SUCCESS : IOSC_FAIL_CHECKVALUE;
  }

  if (signer->subtype != ObjectSubType::RSA2048 && signer->subtype != ObjectSubType::RSA4096)
    return IOSC_INVALID_OBJTYPE;

  const size_t modulus_size = signer->subtype == ObjectSubType::RSA2048 ? 0x100 : 0x200;
  ASSERT(signer->data.size() == modulus_size);
  ASSERT(signature.size() == modulus_size);

  mbedtls_rsa_context rsa;
  mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);
  Common::ScopeGuard free_rsa{[&rsa] { mbedtls_rsa_free(&rsa); }};
  mbedtls_mpi_read_binary(&rsa.N, signer->data.data(), signer->data.size());
  mbedtls_mpi_read_binary(&rsa.E, reinterpret_cast<const u8*>(&signer->misc_data),
                          sizeof(signer->misc_data));
  rsa.len = signer->data.size();

  int status = mbedtls_rsa_pkcs1_verify(&rsa, nullptr, nullptr, MBEDTLS_RSA_PUBLIC,
                                        MBEDTLS_MD_SHA1, 0, sha1.data(), signature.data());

  // Development signatures are not always PKCS#1 padded. On those consoles, accept a signature
  // whose raw RSA result simply ends in the digest.
  if (status != 0 && m_console_type == ConsoleType::RVT)
  {
    std::vector<u8> raw(signature.size());
    const bool decoded = mbedtls_rsa_public(&rsa, signature.data(), raw.data()) == 0;
    if (decoded && std::equal(sha1.begin(), sha1.end(), raw.end() - static_cast<std::ptrdiff_t>(sha1.size())))
      status = 0;
  }

  if (status != 0)
  {
    WARN_LOG_FMT(IOS, "RSA signature check failed (mbedtls error {})", status);
    return IOSC_FAIL_CHECKVALUE;
  }
  return IPC_SUCCESS;
}

ReturnCode IOSC::ImportCertificate(const ES::CertReader& cert, Handle signer_handle,
                                   Handle dest_handle, u32 pid)
{
  if (!HasOwnership(signer_handle, pid) || !HasOwnership(dest_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* signer = FindEntry(signer_handle, SearchMode::IncludeRootKey);
  const KeyEntry* dest = FindEntry(dest_handle, SearchMode::IncludeRootKey);
  if (signer == nullptr || dest == nullptr)
    return IOSC_EINVAL;
  if (signer->type != TYPE_PUBLIC_KEY || dest->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  if (!cert.IsValid())
    return IOSC_INVALID_FORMAT;

  // RetroPixels has no root key of its own. Unless the owner has given one (root_key in
  // wii_keys.txt), the root's signature on a CA certificate is taken on trust; every signature
  // below it (the CA's on the issuer, the issuer's on the ticket or TMD) is still checked.
  const bool root_unchecked = signer_handle == HANDLE_ROOT_KEY && m_root_key_entry.data.empty();
  if (root_unchecked)
  {
    static bool s_logged = false;
    if (!s_logged)
    {
      WARN_LOG_FMT(IOS, "No root key given: CA certificates are accepted without checking the "
                        "root signature");
      s_logged = true;
    }
  }
  else if (VerifyPublicKeySign(cert.GetSha1(), signer_handle, cert.GetSignatureData(), pid) !=
           IPC_SUCCESS)
  {
    return IOSC_FAIL_CHECKVALUE;
  }

  // For RSA keys the exponent is the last word of the key data.
  const std::vector<u8> key = cert.GetPublicKey();
  const u8* exponent = nullptr;
  if (cert.GetSignatureType() != SignatureType::ECC)
    exponent = key.data() + key.size() - 4;
  return ImportPublicKey(dest_handle, key.data(), exponent, pid);
}

// --------------------------------------------------------------------------------------------
// Console identity

// Without the owner's keys there is no console identity: online services stay off, as they did
// for the shared stand-in identity before.
bool IOSC::IsUsingDefaultId() const
{
  return !m_has_console_keys;
}

u32 IOSC::GetDeviceId() const
{
  return m_key_entries[HANDLE_CONSOLE_ID].misc_data;
}

CertECC IOSC::GetDeviceCertificate() const
{
  const std::string issuer = fmt::format("Root-CA{:08x}-MS{:08x}", m_ca_id, m_ms_id);
  const std::string name = fmt::format("NG{:08x}", GetDeviceId());
  CertECC cert = MakeEccCertificate(issuer, name, m_key_entries[HANDLE_CONSOLE_KEY].data.data(),
                                    m_console_key_id);
  cert.signature.sig = m_console_signature;
  return cert;
}

// Signs data the way ES_Sign does: a fresh "AP" key is certified by the console key, and the
// data is signed with the AP key. The AP private key is fixed (the value 1) rather than random.
void IOSC::Sign(u8* sig_out, u8* ap_cert_out, u64 title_id, const u8* data, u32 data_size) const
{
  std::array<u8, 30> ap_private_key{};
  ap_private_key.back() = 1;

  const std::string issuer =
      fmt::format("Root-CA{:08x}-MS{:08x}-NG{:08x}", m_ca_id, m_ms_id, GetDeviceId());
  const std::string name = fmt::format("AP{:016x}", title_id);
  CertECC ap_cert = MakeEccCertificate(issuer, name, ap_private_key.data(), 0);

  // The certificate's signature covers everything from the issuer field on.
  const size_t signed_from = offsetof(CertECC, signature.issuer);
  const auto* cert_bytes = reinterpret_cast<const u8*>(&ap_cert);
  const auto cert_digest =
      Common::SHA1::CalculateDigest(cert_bytes + signed_from, sizeof(ap_cert) - signed_from);
  ap_cert.signature.sig =
      Common::ec::Sign(m_key_entries[HANDLE_CONSOLE_KEY].data.data(), cert_digest.data());
  std::memcpy(ap_cert_out, &ap_cert, sizeof(ap_cert));

  const auto data_digest = Common::SHA1::CalculateDigest(data, data_size);
  const Common::ec::Signature data_sig = Common::ec::Sign(ap_private_key.data(), data_digest.data());
  std::copy(data_sig.begin(), data_sig.end(), sig_out);
}

// --------------------------------------------------------------------------------------------
// Initial contents

void IOSC::LoadDefaultEntries()
{
  // RetroPixels has no keys of its own. Until the owner's key files are read, every key object is
  // empty, so anything that needs one fails cleanly (IOSC_FAIL_INTERNAL). Only the objects IOS
  // itself fills with zeros (NAND key and HMAC, PRNG key) and the console key are zero-filled;
  // the console key is all zero so that a device certificate can still be built.
  m_key_entries[HANDLE_CONSOLE_KEY] = {TYPE_SECRET_KEY, ObjectSubType::ECC233, std::vector<u8>(30),
                                       3};
  m_console_signature = {};
  m_console_key_id = 0;
  m_key_entries[HANDLE_CONSOLE_ID] = {TYPE_DATA, ObjectSubType::Data, {}, 0, 0xFFFFFFF};
  m_key_entries[HANDLE_FS_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128,
                                  std::vector<u8>(AES128_KEY_SIZE), 5};
  m_key_entries[HANDLE_FS_MAC] = {TYPE_SECRET_KEY, ObjectSubType::MAC, std::vector<u8>(20), 5};

  // Retail and development consoles differ in common key, root key and the CA/MS numbers.
  // (Retail key blobs come from CA00000001, which is used here although IOSC itself defaults
  // to 2.)
  const bool dev = m_console_type == ConsoleType::RVT;
  m_key_entries[HANDLE_COMMON_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128, {}, 3};
  m_root_key_entry = {TYPE_PUBLIC_KEY, ObjectSubType::RSA4096, {}, RSA_EXPONENT_65537_BE, 0};
  m_ms_id = dev ? DEV_MS_ID : RETAIL_MS_ID;
  m_ca_id = dev ? DEV_CA_ID : RETAIL_CA_ID;

  m_key_entries[HANDLE_PRNG_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128,
                                    std::vector<u8>(AES128_KEY_SIZE), 3};
  m_key_entries[HANDLE_SD_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128, {}, 3};

  for (const Handle version_slot :
       {HANDLE_BOOT2_VERSION, HANDLE_UNKNOWN_8, HANDLE_UNKNOWN_9, HANDLE_FS_VERSION})
  {
    m_key_entries[version_slot] = {TYPE_DATA, ObjectSubType::Version, {}, 3};
  }

  m_key_entries[HANDLE_NEW_COMMON_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128, {}, 3};
}

void IOSC::LoadEntries()
{
  const bool dev = m_console_type == ConsoleType::RVT;

  // Keys that are not in a console's OTP or SEEPROM come from wii_keys.txt.
  if (auto root_key = LoadOwnerKey(dev ? "root_key_dev" : "root_key", 512))
    m_root_key_entry.data = std::move(*root_key);
  if (auto sd_key = LoadOwnerKey("sd_key", AES128_KEY_SIZE))
    m_key_entries[HANDLE_SD_KEY].data = std::move(*sd_key);

  const std::optional<OwnerConsoleKeys> keys = ReadOwnerConsoleKeys();
  if (!keys)
  {
    WARN_LOG_FMT(IOS,
                 "No keys file from the owner's Wii ({} or {} in {}): Wii titles can't be decrypted",
                 KEYS_BIN_NAME, OTP_BIN_NAME, NandRoot());
    return;
  }
  INFO_LOG_FMT(IOS, "Using the console keys in {}", keys->path);
  m_has_console_keys = true;

  const KeysBin& dump = keys->dump;
  const auto* raw = reinterpret_cast<const u8*>(&dump);
  m_key_entries[HANDLE_CONSOLE_KEY].data.assign(dump.ng_priv.begin(), dump.ng_priv.end());
  m_console_signature = dump.ng_sig;
  m_ms_id = Common::swap32(dump.ms_id);
  m_ca_id = Common::swap32(dump.ca_id);
  m_console_key_id = Common::swap32(dump.ng_key_id);
  m_key_entries[HANDLE_CONSOLE_ID].misc_data = Common::swap32(dump.ng_id);
  m_key_entries[HANDLE_FS_KEY].data.assign(dump.nand_key.begin(), dump.nand_key.end());
  m_key_entries[HANDLE_FS_MAC].data.assign(raw + KEYS_BIN_NAND_HMAC_OFFSET,
                                           raw + KEYS_BIN_NAND_HMAC_OFFSET +
                                               KEYS_BIN_NAND_HMAC_SIZE);
  m_key_entries[HANDLE_PRNG_KEY].data.assign(dump.backup_key.begin(), dump.backup_key.end());
  m_key_entries[HANDLE_BOOT2_VERSION].misc_data = dump.counters[0].boot2version;

  // A console's common keys only decrypt titles for its own kind of console (retail or
  // development). For the other kind they would only give garbage, so they are left out.
  if (ConsoleTypeOfKeys(dump) != m_console_type)
  {
    WARN_LOG_FMT(IOS, "The keys in {} are from a {} console; this title is for a {} console",
                 keys->path, dev ? "retail" : "development", dev ? "development" : "retail");
    return;
  }
  m_key_entries[HANDLE_COMMON_KEY].data.assign(dump.common_key.begin(), dump.common_key.end());
  if (!IsAllZero(dump.korean_key))
  {
    m_key_entries[HANDLE_NEW_COMMON_KEY].data.assign(dump.korean_key.begin(),
                                                     dump.korean_key.end());
  }
}

// --------------------------------------------------------------------------------------------
// The owner's key files

bool IOSC::HasConsoleKeys()
{
  return ReadOwnerConsoleKeys().has_value();
}

bool IOSC::HasConsoleKeys(ConsoleType console_type)
{
  const std::optional<OwnerConsoleKeys> keys = ReadOwnerConsoleKeys();
  return keys && ConsoleTypeOfKeys(keys->dump) == console_type;
}

std::optional<std::vector<u8>> IOSC::LoadOwnerKey(std::string_view name, size_t size)
{
  const std::string path = NandRoot() + KEYS_TEXT_NAME;
  std::string text;
  if (!File::Exists(path) || !File::ReadFileToString(path, text))
    return std::nullopt;
  std::optional<std::vector<u8>> bytes = ParseKeyEntry(text, name, path);
  if (bytes && size != 0 && bytes->size() != size)
  {
    ERROR_LOG_FMT(IOS, "{}: \"{}\" must be {} bytes, not {}; ignored", path, name, size,
                  bytes->size());
    return std::nullopt;
  }
  return bytes;
}

void IOSC::ReportMissingKeys(ConsoleType console_type)
{
  const std::string root = NandRoot();
  if (!HasConsoleKeys())
  {
    PanicAlertFmtT("RetroPixels needs a keys file from your own Wii to play Wii discs, WADs and "
                   "channels.\n\nMake a NAND backup of your Wii with BootMii and copy its keys.bin "
                   "(or otp.bin) to:\n{0}\n\nRetroPixels doesn't include Nintendo's keys and never "
                   "downloads them.",
                   root);
  }
  else if (console_type == ConsoleType::RVT)
  {
    PanicAlertFmtT("This title is for development consoles, and the keys file in {0} is from a "
                   "retail Wii. Put the keys.bin of your own development console there to play it.",
                   root);
  }
  else
  {
    PanicAlertFmtT("This title is for retail consoles, and the keys file in {0} is from a "
                   "development console. Put the keys.bin of your own Wii there to play it.",
                   root);
  }
}

void IOSC::DoState(PointerWrap& p)
{
  for (KeyEntry& entry : m_key_entries)
    entry.DoState(p);
  p.Do(m_console_signature);
  p.Do(m_ms_id);
  p.Do(m_ca_id);
  p.Do(m_console_key_id);
}
}  // namespace IOS::HLE
