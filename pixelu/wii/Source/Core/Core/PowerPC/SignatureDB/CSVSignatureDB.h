// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/PowerPC/SignatureDB/SignatureDB.h"

// Tab-separated text: checksum, size, name, [object location,] object name.
class CSVSignatureDB final : public HashSignatureDB
{
public:
  ~CSVSignatureDB() override = default;
  bool Load(const std::string& file_path) override;
  bool Save(const std::string& file_path) const override;
};
