// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/PowerPC/SignatureDB/SignatureDB.h"

// Binary: u32 count, then count records of {u32 checksum, u32 size,
// char name[128]} in host byte order.
class DSYSignatureDB final : public HashSignatureDB
{
public:
  ~DSYSignatureDB() override = default;
  bool Load(const std::string& file_path) override;
  bool Save(const std::string& file_path) const override;
};
