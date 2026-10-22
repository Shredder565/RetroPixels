// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// A model of the IOS crypto engine (IOSC): a small table of key objects, addressed by handle,
// plus the operations that use them. Only keys that are actually known are supported.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/ec.h"

class PointerWrap;

namespace IOS
{
namespace ES
{
class CertReader;
}  // namespace ES

// Signature and key type words used in tickets, TMDs and certificates (WiiBrew,
// "Certificate chain"). All of these structures are stored big endian.
enum class SignatureType : u32
{
  RSA4096 = 0x00010000,
  RSA2048 = 0x00010001,
  ECC = 0x00010002,
};

enum class PublicKeyType : u32
{
  RSA4096 = 0,
  RSA2048 = 1,
  ECC = 2,
};

#pragma pack(push, 4)
struct SignatureRSA4096
{
  SignatureType type;
  u8 sig[0x200];
  u8 fill[0x3c];
  char issuer[0x40];
};
static_assert(sizeof(SignatureRSA4096) == 0x280, "Wrong size for SignatureRSA4096");

struct SignatureRSA2048
{
  SignatureType type;
  u8 sig[0x100];
  u8 fill[0x3c];
  char issuer[0x40];
};
static_assert(sizeof(SignatureRSA2048) == 0x180, "Wrong size for SignatureRSA2048");

struct SignatureECC
{
  SignatureType type;
  Common::ec::Signature sig;
  u8 fill[0x40];
  char issuer[0x40];
};
static_assert(sizeof(SignatureECC) == 0xc0, "Wrong size for SignatureECC");

// Follows the signature in every certificate.
struct CertHeader
{
  PublicKeyType public_key_type;
  char name[0x40];
  u32 id;
};

using RSA2048PublicKey = std::array<u8, 0x100>;

struct CertRSA4096RSA2048
{
  SignatureRSA4096 signature;
  CertHeader header;
  RSA2048PublicKey public_key;
  u8 exponent[0x4];
  u8 pad[0x34];
};
static_assert(sizeof(CertRSA4096RSA2048) == 0x400, "Wrong size for CertRSA4096RSA2048");

struct CertRSA2048RSA2048
{
  SignatureRSA2048 signature;
  CertHeader header;
  RSA2048PublicKey public_key;
  u8 exponent[0x4];
  u8 pad[0x34];
};
static_assert(sizeof(CertRSA2048RSA2048) == 0x300, "Wrong size for CertRSA2048RSA2048");

// A console (NG) certificate: RSA-signed, ECC key.
struct CertRSA2048ECC
{
  SignatureRSA2048 signature;
  CertHeader header;
  Common::ec::PublicKey public_key;
  std::array<u8, 60> padding;
};
static_assert(sizeof(CertRSA2048ECC) == 0x240, "Wrong size for CertRSA2048ECC");

// A certificate signed by a console key (for example an AP certificate).
struct CertECC
{
  SignatureECC signature;
  CertHeader header;
  Common::ec::PublicKey public_key;
  std::array<u8, 60> padding;
};
static_assert(sizeof(CertECC) == 0x180, "Wrong size for CertECC");
#pragma pack(pop)

namespace HLE
{
enum ReturnCode : s32;

class IOSC final
{
public:
  using Handle = u32;

  enum class ConsoleType
  {
    Retail,
    RVT,
  };

  // Built-in objects and their fixed handles. Titles pass these numbers in ioctlvs, so they
  // have to match the real IOSC (WiiBrew, "IOS/Syscalls").
  enum DefaultHandle : u32
  {
    // The console's ECC-233 private key (NG key)
    HANDLE_CONSOLE_KEY = 0,
    // Console ID
    HANDLE_CONSOLE_ID = 1,
    // NAND encryption key (AES-128)
    HANDLE_FS_KEY = 2,
    // NAND HMAC key
    HANDLE_FS_MAC = 3,
    // Common key
    HANDLE_COMMON_KEY = 4,
    // PRNG seed
    HANDLE_PRNG_KEY = 5,
    // SD card key (AES-128)
    HANDLE_SD_KEY = 6,
    // boot2 version (writable)
    HANDLE_BOOT2_VERSION = 7,
    HANDLE_UNKNOWN_8 = 8,
    HANDLE_UNKNOWN_9 = 9,
    // File system version (writable)
    HANDLE_FS_VERSION = 10,
    // Second common key, used for Korean titles
    HANDLE_NEW_COMMON_KEY = 11,

    HANDLE_ROOT_KEY = 0xfffffff,
  };

  static constexpr std::array<DefaultHandle, 2> COMMON_KEY_HANDLES = {HANDLE_COMMON_KEY,
                                                                      HANDLE_NEW_COMMON_KEY};

  enum ObjectType : u8
  {
    TYPE_SECRET_KEY = 0,
    TYPE_PUBLIC_KEY = 1,
    TYPE_DATA = 3,
  };

  enum class ObjectSubType : u8
  {
    AES128 = 0,
    MAC = 1,
    RSA2048 = 2,
    RSA4096 = 3,
    ECC233 = 4,
    Data = 5,
    Version = 6
  };

  IOSC(ConsoleType console_type = ConsoleType::Retail);
  ~IOSC();

  // Allocates an empty object owned by `pid`.
  ReturnCode CreateObject(Handle* handle, ObjectType type, ObjectSubType subtype, u32 pid);
  // Frees an object. Built-in objects cannot be freed.
  ReturnCode DeleteObject(Handle handle, u32 pid);
  // Decrypts `encrypted_key` with the key in decrypt_handle and stores it in dest_handle.
  ReturnCode ImportSecretKey(Handle dest_handle, Handle decrypt_handle, u8* iv,
                             const u8* encrypted_key, u32 pid);
  // Stores a plain AES key in dest_handle.
  ReturnCode ImportSecretKey(Handle dest_handle, const u8* decrypted_key, u32 pid);
  // Stores a public key; RSA keys also need their exponent.
  ReturnCode ImportPublicKey(Handle dest_handle, const u8* public_key,
                             const u8* public_key_exponent, u32 pid);
  // Derives an AES key from an ECDH exchange between a private and a public ECC key.
  ReturnCode ComputeSharedKey(Handle dest_handle, Handle private_handle, Handle public_handle,
                              u32 pid);

  // AES-128-CBC. `iv` is updated for chaining.
  ReturnCode Encrypt(Handle key_handle, u8* iv, const u8* input, size_t size, u8* output,
                     u32 pid) const;
  ReturnCode Decrypt(Handle key_handle, u8* iv, const u8* input, size_t size, u8* output,
                     u32 pid) const;

  ReturnCode VerifyPublicKeySign(const std::array<u8, 20>& sha1, Handle signer_handle,
                                 const std::vector<u8>& signature, u32 pid) const;
  // Checks `cert` against the key in signer_handle, then stores its public key in dest_handle.
  ReturnCode ImportCertificate(const ES::CertReader& cert, Handle signer_handle, Handle dest_handle,
                               u32 pid);

  // Owner masks (one bit per process ID).
  ReturnCode GetOwnership(Handle handle, u32* owner) const;
  ReturnCode SetOwnership(Handle handle, u32 owner, u32 pid);

  // True when there is no console identity (no keys file from the owner's Wii).
  bool IsUsingDefaultId() const;
  u32 GetDeviceId() const;
  CertECC GetDeviceCertificate() const;
  void Sign(u8* sig_out, u8* ap_cert_out, u64 title_id, const u8* data, u32 data_size) const;

  void DoState(PointerWrap& p);

  // The owner's key files. RetroPixels has no Nintendo keys of its own: a console's keys come
  // from a dump of the owner's own console in the Wii NAND root, either keys.bin (BootMii's 1 KiB
  // OTP/SEEPROM dump) or otp.bin (a Wii's 128-byte OTP, or a Wii U's 1 KiB OTP, whose first bank
  // is the vWii's Wii OTP). Keys that are not in a console's OTP (the SD card key and IV, for
  // example) can be given as "name = hex" lines in wii_keys.txt next to them.
  static bool HasConsoleKeys();
  // Also checks that the keys are from the given kind of console (retail or development).
  static bool HasConsoleKeys(ConsoleType console_type);
  // A named entry from wii_keys.txt, if it is there and `size` bytes long (any length for 0).
  static std::optional<std::vector<u8>> LoadOwnerKey(std::string_view name, size_t size);
  // Tells the player that the keys file is missing (or from the other kind of console).
  static void ReportMissingKeys(ConsoleType console_type);

private:
  struct KeyEntry
  {
    KeyEntry();
    KeyEntry(ObjectType type_, ObjectSubType subtype_, std::vector<u8>&& data_, u32 owner_mask_);
    KeyEntry(ObjectType type_, ObjectSubType subtype_, std::vector<u8>&& data_, u32 misc_data_,
             u32 owner_mask_);
    void DoState(PointerWrap& p);

    bool in_use = false;
    ObjectType type{};
    ObjectSubType subtype{};
    std::vector<u8> data;
    u32 misc_data = 0;
    u32 owner_mask = 0;
  };
  // 32 slots, the first 12 of which hold the built-in objects.
  using KeyEntries = std::array<KeyEntry, 32>;

  enum class SearchMode
  {
    IncludeRootKey,
    ExcludeRootKey,
  };

  void LoadDefaultEntries();
  void LoadEntries();

  KeyEntries::iterator FindFreeEntry();
  KeyEntry* FindEntry(Handle handle);
  const KeyEntry* FindEntry(Handle handle, SearchMode mode = SearchMode::ExcludeRootKey) const;

  Handle GetHandleFromIterator(KeyEntries::iterator iterator) const;
  bool HasOwnership(Handle handle, u32 pid) const;
  bool IsDefaultHandle(Handle handle) const;
  ReturnCode DecryptEncrypt(Common::AES::Mode mode, Handle key_handle, u8* iv, const u8* input,
                            size_t size, u8* output, u32 pid) const;

  ConsoleType m_console_type{ConsoleType::Retail};
  KeyEntries m_key_entries;
  // The root CA key lives outside the table and is never saved.
  KeyEntry m_root_key_entry;
  Common::ec::Signature m_console_signature{};
  u32 m_ms_id = 0;
  u32 m_ca_id = 0;
  u32 m_console_key_id = 0;
  // Whether the owner's console keys were found (not saved in savestates).
  bool m_has_console_keys = false;
};
}  // namespace HLE
}  // namespace IOS
