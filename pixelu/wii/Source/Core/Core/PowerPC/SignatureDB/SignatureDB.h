// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Function signature databases: recognise library functions in guest code
// and name them. SignatureDB picks a file format handler (DSY binary, CSV,
// or MEGA pattern files) and forwards to it. These objects are meant to be
// short-lived: create, load/apply or populate/save, discard.

#pragma once

#include <map>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

class PPCSymbolDB;
class SignatureDBFormatHandler;

class SignatureDB
{
public:
  enum class HandlerType
  {
    DSY,
    CSV,
    MEGA
  };
  explicit SignatureDB(HandlerType handler);
  // Chooses the handler from the extension: .csv, .mega, otherwise DSY.
  explicit SignatureDB(const std::string& file_path);

  void Clear();
  // Adds to the current contents; call Clear() first to replace them.
  bool Load(const std::string& file_path);
  bool Save(const std::string& file_path) const;
  void List() const;

  // Takes signatures from the symbols in func_db (named ones, or those whose
  // name starts with filter).
  void Populate(const PPCSymbolDB* func_db, const std::string& filter = "");
  // Renames the symbols in func_db that match a signature.
  void Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* func_db) const;

  bool Add(const Core::CPUThreadGuard& guard, u32 start_addr, u32 size, const std::string& name);

private:
  std::unique_ptr<SignatureDBFormatHandler> m_handler;
};

class SignatureDBFormatHandler
{
public:
  virtual ~SignatureDBFormatHandler();

  virtual void Clear() = 0;
  virtual bool Load(const std::string& file_path) = 0;
  virtual bool Save(const std::string& file_path) const = 0;
  virtual void List() const = 0;

  virtual void Populate(const PPCSymbolDB* func_db, const std::string& filter = "") = 0;
  virtual void Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* func_db) const = 0;

  virtual bool Add(const Core::CPUThreadGuard& guard, u32 startAddr, u32 size,
                   const std::string& name) = 0;
};

// Formats that identify a function by a checksum of its instruction opcodes.
class HashSignatureDB : public SignatureDBFormatHandler
{
public:
  struct DBFunc
  {
    u32 size = 0;
    std::string name;
    std::string object_name;
    std::string object_location;
  };
  using FuncDB = std::map<u32, DBFunc>;

  // Checksum of the words in [offsetStart, offsetEnd] (inclusive) that only
  // looks at opcode bits, so that it survives relinking and register
  // allocation differences between builds of the same code.
  static u32 ComputeCodeChecksum(const Core::CPUThreadGuard& guard, u32 offsetStart, u32 offsetEnd);

  void Clear() override;
  void List() const override;

  void Populate(const PPCSymbolDB* func_db, const std::string& filter = "") override;
  void Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* func_db) const override;

  bool Add(const Core::CPUThreadGuard& guard, u32 startAddr, u32 size,
           const std::string& name) override;

protected:
  // Keyed by checksum.
  FuncDB m_database;
};
