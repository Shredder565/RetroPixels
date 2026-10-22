// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"

namespace Core
{
class CPUThreadGuard;
}

class PPCSymbolDB;

// A symbol referenced from inside a signature, at a byte offset.
struct MEGASignatureReference
{
  MEGASignatureReference(u32 ref_offset, std::string ref_name)
      : offset(ref_offset), name(std::move(ref_name))
  {
  }
  u32 offset;
  std::string name;
};

struct MEGASignature
{
  std::vector<u32> code;  // instruction words; 0 matches anything
  std::string name;
  std::vector<MEGASignatureReference> refs;
};

// Pattern files as produced by the WiiTools IDA plugin
// (https://github.com/Megazig/WiiTools). One signature per line:
//   <hex words, "........" = wildcard word> <:offset> <name> [^offset refname]...
// Matching compares a symbol's code word by word. Read-only format.
class MEGASignatureDB : public SignatureDBFormatHandler
{
public:
  MEGASignatureDB();
  ~MEGASignatureDB() override;

  void Clear() override;
  bool Load(const std::string& file_path) override;
  bool Save(const std::string& file_path) const override;
  void List() const override;

  void Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* symbol_db) const override;
  void Populate(const PPCSymbolDB* func_db, const std::string& filter = "") override;

  bool Add(const Core::CPUThreadGuard& guard, u32 startAddr, u32 size,
           const std::string& name) override;

private:
  std::vector<MEGASignature> m_signatures;
};
