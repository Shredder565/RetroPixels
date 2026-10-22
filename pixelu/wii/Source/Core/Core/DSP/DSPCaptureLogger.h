// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Hooks through which the DSP reports its hardware register traffic and DMA
// transfers, so that they can be recorded for later analysis.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
class PCAP;
}

namespace DSP
{
// Called from the DSP's hottest paths: implementations should be quick.
class DSPCaptureLogger
{
public:
  virtual ~DSPCaptureLogger() = default;

  // 16-bit accesses to the hardware registers at 0xffxx.
  virtual void LogIFXRead(u16 address, u16 read_value) = 0;
  virtual void LogIFXWrite(u16 address, u16 written_value) = 0;

  // One DMA between main memory and DSP memory, in either direction and for
  // either DSP memory. control is the raw DMA control register; length is in bytes.
  virtual void LogDMA(u16 control, u32 gc_address, u16 dsp_address, u16 length, const u8* data) = 0;
};

// Records nothing. Also a convenient base for loggers interested in only some events.
class DefaultDSPCaptureLogger : public DSPCaptureLogger
{
public:
  void LogIFXRead(u16, u16) override {}
  void LogIFXWrite(u16, u16) override {}
  void LogDMA(u16, u32, u16, u16, const u8*) override {}
};

// Writes every event as one packet of a PCAP capture file.
class PCAPDSPCaptureLogger final : public DSPCaptureLogger
{
public:
  // Creates (or truncates) the named file.
  explicit PCAPDSPCaptureLogger(const std::string& pcap_filename);
  // Both of these take ownership of the capture.
  explicit PCAPDSPCaptureLogger(Common::PCAP* pcap);
  explicit PCAPDSPCaptureLogger(std::unique_ptr<Common::PCAP>&& pcap);
  ~PCAPDSPCaptureLogger() override;

  void LogIFXRead(u16 address, u16 read_value) override;
  void LogIFXWrite(u16 address, u16 written_value) override;
  void LogDMA(u16 control, u32 gc_address, u16 dsp_address, u16 length, const u8* data) override;

private:
  void WriteRegisterAccess(bool is_read, u16 address, u16 value);

  std::unique_ptr<Common::PCAP> m_pcap;
  std::vector<u8> m_packet;  // reused for DMA packets
};
}  // namespace DSP
