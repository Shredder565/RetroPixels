// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/WorkQueueThread.h"
#include "Core/HW/EXI/EXI_Device.h"

struct cubeb;
struct cubeb_stream;

namespace ExpansionInterface
{
// The GameCube microphone (DOL-022) in a memory card slot. Software writes a 16-bit status word
// to start sampling; the mic then raises an EXI interrupt each time its ring buffer has filled,
// and software reads the buffer out. Audio comes from the host's default input via cubeb.
class CEXIMic : public IEXIDevice
{
public:
  CEXIMic(Core::System& system, const int index);
  ~CEXIMic() override;
  void SetCS(int cs) override;
  bool IsInterruptSet() override;
  bool IsPresent() const override;

private:
  void TransferByte(u8& byte) override;

  void ReadStatusByte(u32 index, u8& byte);
  void WriteStatusByte(u32 index, u8 byte);
  u8 NextBufferByte();

  void ScheduleNextInterrupt();

  // Capture side (cubeb).
  void OpenContext();
  void CloseContext();
  void StartCapture();
  void StopCapture();
  void CopyCapturedBlock();
  static long OnCapturedFrames(cubeb_stream* stream, void* user_data, const void* input_buffer,
                               void* output_buffer, long nframes);
  // Runs cubeb calls where they have to run (a COM-initialised thread on Windows). Returns
  // false if they cannot run there.
  bool RunAudioTask(const std::function<void()>& task, bool needs_com);

  int m_slot;

  u32 m_position = 0;
  int m_command = 0;
  // Status word: bits 0-3 output, 4 ID, 5-7 unused buttons, 8 button, 9 overflow, 10 gain,
  // 11-12 sample rate, 13-14 ring size, 15 active. Transferred high byte first.
  u16 m_status = 0;

  // Derived from the status word when sampling starts.
  int m_sample_rate = 0;
  u32 m_ring_bytes = 0;
  u32 m_ring_samples = 0;

  // The block software reads (up to 32 << 3 bytes) and the read position in it.
  std::array<u8, 256> m_ring{};
  u32 m_ring_pos = 0;

  // When the next "buffer full" interrupt is due, or 0 while none is.
  u64 m_next_irq_tick = 0;

  // Captured samples waiting to be handed to the console, written by the cubeb thread.
  std::mutex m_capture_lock;
  std::vector<s16> m_capture;
  std::size_t m_capture_write = 0;
  std::size_t m_capture_read = 0;
  std::size_t m_capture_available = 0;

  std::shared_ptr<cubeb> m_cubeb_ctx = nullptr;
  cubeb_stream* m_cubeb_stream = nullptr;

#ifdef _WIN32
  Common::AsyncWorkThread m_work_queue;
  bool m_coinit_success = false;
  bool m_should_couninit = false;
#endif
};
}  // namespace ExpansionInterface
