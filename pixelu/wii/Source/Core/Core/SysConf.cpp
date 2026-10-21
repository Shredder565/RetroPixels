// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/SysConf.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"

namespace
{
namespace FS = IOS::HLE::FS;

constexpr size_t FILE_SIZE = 0x4000;
constexpr std::array<u8, 4> HEADER_MAGIC{'S', 'C', 'v', '0'};
constexpr std::array<u8, 4> FOOTER_MAGIC{'S', 'C', 'e', 'd'};

constexpr const char* SYSCONF_PATH = "/shared2/sys/SYSCONF";
constexpr const char* SYSCONF_DIR = "/shared2/sys";
// Written here first and then renamed over the real file.
constexpr const char* STAGING_PATH = "/tmp/SYSCONF";

// Data size of the scalar types; arrays carry their own length.
size_t ScalarSize(SysConf::Entry::Type type)
{
  using Type = SysConf::Entry::Type;
  switch (type)
  {
  case Type::Byte:
  case Type::ByteBool:
    return sizeof(u8);
  case Type::Short:
    return sizeof(u16);
  case Type::Long:
    return sizeof(u32);
  case Type::LongLong:
    return sizeof(u64);
  default:
    ASSERT(false);
    return 0;
  }
}

bool IsArray(SysConf::Entry::Type type)
{
  return type == SysConf::Entry::Type::BigArray || type == SysConf::Entry::Type::SmallArray;
}

// Appends `value` to `out` in big-endian order.
template <typename T>
void PutBE(std::vector<u8>& out, T value)
{
  const T raw = Common::FromBigEndian(value);
  const auto* raw_bytes = reinterpret_cast<const u8*>(&raw);
  out.insert(out.end(), raw_bytes, raw_bytes + sizeof(T));
}
}  // namespace

SysConf::SysConf(std::shared_ptr<IOS::HLE::FS::FileSystem> fs) : m_fs{std::move(fs)}
{
  Load();
}

SysConf::~SysConf()
{
  Save();
}

void SysConf::Clear()
{
  m_entries.clear();
}

void SysConf::Load()
{
  Clear();

  const auto file = m_fs->OpenFile(FS::Uid{0}, FS::Gid{0}, SYSCONF_PATH, FS::Mode::Read);
  const bool usable = file && file->GetStatus()->size == FILE_SIZE && LoadFromFile(*file);
  if (!usable)
  {
    WARN_LOG_FMT(CORE, "No valid SYSCONF detected. Creating a new one.");
    InsertDefaultEntries();
  }
}

bool SysConf::LoadFromFile(const IOS::HLE::FS::FileHandle& file)
{
  // Read errors are not checked: a damaged file simply yields odd entries,
  // the way the System Menu treats it. Only an unknown type rejects the file.
  const auto read_be16 = [&file] {
    u16 raw = 0;
    (void)file.Read(&raw, 1);
    return Common::swap16(raw);
  };

  (void)file.Seek(static_cast<u32>(HEADER_MAGIC.size()), FS::SeekMode::Set);
  const u16 count = read_be16();

  std::vector<u16> offsets(count);
  for (u16& offset : offsets)
    offset = read_be16();

  for (const u16 offset : offsets)
  {
    (void)file.Seek(offset, FS::SeekMode::Set);

    u8 descriptor = 0;
    (void)file.Read(&descriptor, 1);
    const auto type = static_cast<Entry::Type>(descriptor >> 5);
    std::string name(static_cast<size_t>(descriptor & 0x1f) + 1, '\0');
    (void)file.Read(name.data(), name.size());

    size_t data_size = 0;
    if (type == Entry::Type::BigArray)
    {
      data_size = static_cast<size_t>(read_be16()) + 1;
    }
    else if (type == Entry::Type::SmallArray)
    {
      u8 stored_length = 0;
      (void)file.Read(&stored_length, 1);
      data_size = static_cast<size_t>(stored_length) + 1;
    }
    else if (type >= Entry::Type::Byte && type <= Entry::Type::ByteBool)
    {
      data_size = ScalarSize(type);
    }
    else
    {
      ERROR_LOG_FMT(CORE, "Unknown entry type {} in SYSCONF for {} (offset {})",
                    static_cast<u8>(type), name, offset);
      return false;
    }

    std::vector<u8> data(data_size);
    (void)file.Read(data.data(), data.size());
    AddEntry({type, name, std::move(data)});
  }
  return true;
}

bool SysConf::Save() const
{
  const size_t count = m_entries.size();

  // The offset table holds one offset per entry plus the end offset.
  const size_t table_end = HEADER_MAGIC.size() + sizeof(u16) + sizeof(u16) * (count + 1);

  std::vector<u8> body;
  std::vector<u16> offsets;
  offsets.reserve(count + 1);
  for (const Entry& entry : m_entries)
  {
    offsets.push_back(static_cast<u16>(table_end + body.size()));

    const auto name_length = static_cast<u8>(entry.name.size());
    body.push_back(static_cast<u8>((static_cast<u8>(entry.type) << 5) | (name_length - 1)));
    body.insert(body.end(), entry.name.begin(), entry.name.end());

    // Array lengths are stored minus one and truncated to their field width,
    // and only that many data bytes follow.
    size_t data_size = entry.bytes.size();
    if (entry.type == Entry::Type::BigArray)
    {
      const auto length = static_cast<u16>(data_size);
      PutBE<u16>(body, static_cast<u16>(length - 1));
      data_size = length;
    }
    else if (entry.type == Entry::Type::SmallArray)
    {
      const auto length = static_cast<u8>(data_size);
      PutBE<u8>(body, static_cast<u8>(length - 1));
      data_size = length;
    }
    body.insert(body.end(), entry.bytes.begin(),
                entry.bytes.begin() + static_cast<std::ptrdiff_t>(data_size));
  }
  offsets.push_back(static_cast<u16>(table_end + body.size()));

  std::vector<u8> image(HEADER_MAGIC.begin(), HEADER_MAGIC.end());
  image.reserve(FILE_SIZE);
  PutBE<u16>(image, static_cast<u16>(count));
  for (const u16 offset : offsets)
    PutBE<u16>(image, offset);
  image.insert(image.end(), body.begin(), body.end());

  // Pad (or cut) to the fixed size; the footer always occupies the last bytes.
  image.resize(FILE_SIZE);
  std::ranges::copy(FOOTER_MAGIC, image.end() - static_cast<std::ptrdiff_t>(FOOTER_MAGIC.size()));

  constexpr FS::Modes all_rw{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};
  {
    const auto staging = m_fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID,
                                                 STAGING_PATH, all_rw);
    if (!staging || !staging->Write(image.data(), image.size()))
      return false;
  }

  m_fs->CreateDirectory(IOS::SYSMENU_UID, IOS::SYSMENU_GID, SYSCONF_DIR, 0, all_rw);
  return m_fs->Rename(IOS::SYSMENU_UID, IOS::SYSMENU_GID, STAGING_PATH, SYSCONF_PATH) ==
         FS::ResultCode::Success;
}

SysConf::Entry::Entry(Type type_, std::string name_) : type(type_), name(std::move(name_))
{
  if (!IsArray(type))
    bytes.resize(ScalarSize(type));
}

SysConf::Entry::Entry(Type type_, std::string name_, std::vector<u8> bytes_)
    : type(type_), name(std::move(name_)), bytes(std::move(bytes_))
{
}

SysConf::Entry& SysConf::AddEntry(Entry&& entry)
{
  return m_entries.emplace_back(std::move(entry));
}

SysConf::Entry* SysConf::GetEntry(std::string_view key)
{
  const auto& self = *this;
  return const_cast<Entry*>(self.GetEntry(key));
}

const SysConf::Entry* SysConf::GetEntry(std::string_view key) const
{
  for (const Entry& entry : m_entries)
  {
    if (entry.name == key)
      return &entry;
  }
  return nullptr;
}

SysConf::Entry* SysConf::GetOrAddEntry(std::string_view key, Entry::Type type)
{
  Entry* existing = GetEntry(key);
  return existing ? existing : &AddEntry(Entry{type, std::string(key)});
}

void SysConf::RemoveEntry(std::string_view key)
{
  std::erase_if(m_entries, [key](const Entry& entry) { return entry.name == key; });
}

void SysConf::InsertDefaultEntries()
{
  using Type = Entry::Type;

  // The settings a freshly formatted console has, in the System Menu's order.

  // Bluetooth: paired and connected device tables (empty), sensor bar
  // sensitivity 3, sensor bar below the TV, speaker volume, rumble on.
  AddEntry({Type::BigArray, "BT.DINF", std::vector<u8>(0x461)});
  AddEntry({Type::BigArray, "BT.CDIF", std::vector<u8>(0x205)});
  AddEntry({Type::Long, "BT.SENS", {0, 0, 0, 3}});
  AddEntry({Type::Byte, "BT.BAR", {1}});
  AddEntry({Type::Byte, "BT.SPKV", {0x58}});
  AddEntry({Type::Byte, "BT.MOT", {1}});

  // Console nickname: up to ten UTF-16BE characters and a terminator, with the
  // character count in the last byte.
  {
    constexpr std::string_view nickname = "dolphin";
    std::vector<u8> field(22);
    for (size_t i = 0; i < nickname.size(); ++i)
      field[2 * i + 1] = static_cast<u8>(nickname[i]);
    field.back() = static_cast<u8>(nickname.size());
    AddEntry({Type::SmallArray, "IPL.NIK", std::move(field)});
  }

  AddEntry({Type::Byte, "IPL.LNG", {1}});

  // Simple address (region and country). The first byte is the country code (0x6c).
  {
    std::vector<u8> simple_address(0x1008);
    simple_address.front() = 0x6c;
    AddEntry({Type::BigArray, "IPL.SADR", std::move(simple_address)});
  }

  // Parental controls (off).
  {
    std::vector<u8> parental(0x4a);
    parental[1] = 0x04;
    parental[2] = 0x14;
    AddEntry({Type::SmallArray, "IPL.PC", std::move(parental)});
  }

  AddEntry({Type::Long, "IPL.CB", {0, 0, 0, 0}});
  AddEntry({Type::Byte, "IPL.AR", {1}});
  AddEntry({Type::Byte, "IPL.SSV", {1}});

  AddEntry({Type::ByteBool, "IPL.CD", {0}});
  AddEntry({Type::ByteBool, "IPL.CD2", {0}});
  AddEntry({Type::ByteBool, "IPL.EULA", {1}});
  AddEntry({Type::Byte, "IPL.UPT", {2}});
  AddEntry({Type::Byte, "IPL.PGS", {0}});
  AddEntry({Type::Byte, "IPL.E60", {1}});
  AddEntry({Type::Byte, "IPL.DH", {0}});
  AddEntry({Type::Long, "IPL.INC", {0, 0, 0, 8}});
  AddEntry({Type::Long, "IPL.FRC", {0, 0, 0, 0x28}});
  AddEntry({Type::SmallArray, "IPL.IDL", {0, 1}});

  AddEntry({Type::Long, "NET.WCFG", {0, 0, 0, 1}});
  AddEntry({Type::Long, "NET.CTPC", std::vector<u8>(4)});
  AddEntry({Type::Byte, "WWW.RST", {0}});

  AddEntry({Type::ByteBool, "MPLS.MOVIE", {1}});
}
