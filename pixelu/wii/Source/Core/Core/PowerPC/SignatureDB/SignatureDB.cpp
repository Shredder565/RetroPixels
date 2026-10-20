// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/SignatureDB/SignatureDB.h"

#include <bit>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/SignatureDB/CSVSignatureDB.h"
#include "Core/PowerPC/SignatureDB/DSYSignatureDB.h"
#include "Core/PowerPC/SignatureDB/MEGASignatureDB.h"

namespace
{
std::unique_ptr<SignatureDBFormatHandler> MakeHandler(SignatureDB::HandlerType type)
{
  switch (type)
  {
  case SignatureDB::HandlerType::CSV:
    return std::make_unique<CSVSignatureDB>();
  case SignatureDB::HandlerType::MEGA:
    return std::make_unique<MEGASignatureDB>();
  case SignatureDB::HandlerType::DSY:
  default:
    return std::make_unique<DSYSignatureDB>();
  }
}

SignatureDB::HandlerType HandlerForPath(const std::string& path)
{
  if (path.ends_with(".csv"))
    return SignatureDB::HandlerType::CSV;
  if (path.ends_with(".mega"))
    return SignatureDB::HandlerType::MEGA;
  return SignatureDB::HandlerType::DSY;
}

// The part of an instruction that the checksum keeps: the primary opcode
// plus, depending on the form, the extended opcode or the register fields
// whose values tend to be fixed by the compiler's calling convention.
u32 ChecksumBits(u32 inst)
{
  const u32 primary = inst >> 26;
  u32 kept = inst & 0xFC000000;
  switch (primary)
  {
  case 4:  // paired singles: XO in the low six bits; some forms also keep frC
  {
    const u32 xo = inst & 0x3F;
    kept |= xo;
    if (xo == 0 || xo == 8 || xo == 16 || xo == 21 || xo == 22)
      kept |= inst & 0x7C0;
    break;
  }
  case 7:
  case 8:
  case 10:
  case 11:
  case 12:
  case 13:
  case 14:
  case 15:  // integer immediates: rD/rA
    kept |= inst & 0x03FF0000;
    break;
  case 19:
  case 31:
  case 63:  // extended opcode and Rc
    kept |= inst & 0x7FF;
    break;
  case 59:  // single-precision FP: XO, and frC for the low XOs
    kept |= inst & 0x3F;
    if ((inst & 0x3F) < 16)
      kept |= inst & 0x7C0;
    break;
  default:
    if (primary >= 32 && primary < 56)  // loads and stores: rD/rA
      kept |= inst & 0x03FF0000;
    break;
  }
  return kept;
}
}  // namespace

SignatureDB::SignatureDB(HandlerType handler) : m_handler(MakeHandler(handler))
{
}

SignatureDB::SignatureDB(const std::string& file_path) : SignatureDB(HandlerForPath(file_path))
{
}

void SignatureDB::Clear()
{
  m_handler->Clear();
}

bool SignatureDB::Load(const std::string& file_path)
{
  return m_handler->Load(file_path);
}

bool SignatureDB::Save(const std::string& file_path) const
{
  return m_handler->Save(file_path);
}

void SignatureDB::List() const
{
  m_handler->List();
}

void SignatureDB::Populate(const PPCSymbolDB* func_db, const std::string& filter)
{
  m_handler->Populate(func_db, filter);
}

void SignatureDB::Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* func_db) const
{
  m_handler->Apply(guard, func_db);
}

bool SignatureDB::Add(const Core::CPUThreadGuard& guard, u32 start_addr, u32 size,
                      const std::string& name)
{
  return m_handler->Add(guard, start_addr, size, name);
}

SignatureDBFormatHandler::~SignatureDBFormatHandler() = default;

u32 HashSignatureDB::ComputeCodeChecksum(const Core::CPUThreadGuard& guard, u32 offsetStart,
                                         u32 offsetEnd)
{
  if (offsetEnd < offsetStart)
    return 0;

  // Rotate-and-xor over the words; counted so an end near 4 GiB cannot wrap.
  const u32 words = (offsetEnd - offsetStart) / 4 + 1;
  u32 sum = 0;
  for (u32 i = 0; i < words; ++i)
  {
    const u32 inst = PowerPC::MMU::HostRead_Instruction(guard, offsetStart + i * 4);
    sum = std::rotl(sum, 17) ^ ChecksumBits(inst);
  }
  return sum;
}

bool HashSignatureDB::Add(const Core::CPUThreadGuard& guard, u32 startAddr, u32 size,
                          const std::string& name)
{
  const u32 hash = ComputeCodeChecksum(guard, startAddr, startAddr + size - 4);
  if (m_database.contains(hash))
    return false;

  DBFunc& func = m_database[hash];
  func.size = size;
  func.name = name;
  return true;
}

void HashSignatureDB::List() const
{
  for (const auto& [hash, func] : m_database)
    DEBUG_LOG_FMT(SYMBOLS, "{} : {} bytes, hash = {:08x}", func.name, func.size, hash);
  INFO_LOG_FMT(SYMBOLS, "{} functions known in current database.", m_database.size());
}

void HashSignatureDB::Clear()
{
  m_database.clear();
}

void HashSignatureDB::Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* symbol_db) const
{
  for (const auto& [hash, known] : m_database)
  {
    for (const Common::Symbol* symbol : symbol_db->GetSymbolsFromHash(hash))
    {
      symbol_db->RenameSymbol(*symbol, known.name);
      if (known.size == symbol->size)
      {
        INFO_LOG_FMT(SYMBOLS, "Found {} at {:08x} (size: {:08x})!", known.name, symbol->address,
                     symbol->size);
      }
      else
      {
        ERROR_LOG_FMT(SYMBOLS, "Wrong size! Found {} at {:08x} (size: {:08x} instead of {:08x})!",
                      known.name, symbol->address, symbol->size, known.size);
      }
    }
  }
  symbol_db->Index();
}

void HashSignatureDB::Populate(const PPCSymbolDB* symbol_db, const std::string& filter)
{
  // Without a filter, take every symbol that has a real name (not an
  // auto-generated "zz_" one and not a section name).
  const auto wanted = [&filter](const std::string& name) {
    if (!filter.empty())
      return name.starts_with(filter);
    return !name.empty() && !name.starts_with("zz_") && !name.starts_with(".");
  };

  symbol_db->ForEachSymbol([&](const Common::Symbol& symbol) {
    if (!wanted(symbol.name))
      return;
    DBFunc func;
    func.name = symbol.name;
    func.size = symbol.size;
    m_database[symbol.hash] = std::move(func);
  });
}
