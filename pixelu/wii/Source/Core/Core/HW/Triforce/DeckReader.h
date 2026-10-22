// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DirectIOFile.h"

#include "Core/HW/Triforce/ICCardReader.h"
#include "Core/HW/Triforce/SerialDevice.h"

class PointerWrap;

namespace Triforce
{

// The Key of Avalon's card deck reader on serial port A. Its IC card reader
// sits behind it: IC card requests are forwarded and the reader's replies are
// passed back unchanged. The deck itself is described by a JSON file.
class DeckReader final : public SerialDevice
{
public:
  void Update() override;

  void DoState(PointerWrap& p) override;

  auto* GetICCardReader() { return &m_card_reader; }

private:
  void Step();
  void RunFirmwareMode();
  void RunDeckCommand(u8 command);
  std::vector<u8> ReadDeck() const;

  ICCardReader m_card_reader{0};

  // Nonzero while a (discarded) firmware upload is in progress.
  u8 m_firmware_timeout = 0;

  File::DirectIOFile m_firmware_file;
};

}  // namespace Triforce
