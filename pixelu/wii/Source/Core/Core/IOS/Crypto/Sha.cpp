// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Crypto/Sha.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include <mbedtls/sha1.h>

#include "Common/CommonTypes.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Owns an mbedtls SHA-1 context loaded from (and saved back to) the caller's state block.
class Sha1Engine
{
public:
  explicit Sha1Engine(const ShaDevice::ShaContext& saved)
  {
    // Only the chaining words and the counter travel between requests, so everything else
    // (including mbedtls' partial-block buffer) starts out zeroed.
    mbedtls_sha1_init(&m_ctx);
    std::copy(saved.states.begin(), saved.states.end(), m_ctx.state);
    std::copy(saved.length.begin(), saved.length.end(), m_ctx.total);
  }
  ~Sha1Engine() { mbedtls_sha1_free(&m_ctx); }

  Sha1Engine(const Sha1Engine&) = delete;
  Sha1Engine& operator=(const Sha1Engine&) = delete;

  ShaDevice::ShaContext Save() const
  {
    ShaDevice::ShaContext out{};
    std::copy(std::begin(m_ctx.state), std::end(m_ctx.state), out.states.begin());
    std::copy(std::begin(m_ctx.total), std::end(m_ctx.total), out.length.begin());
    return out;
  }

  mbedtls_sha1_context* get() { return &m_ctx; }

private:
  mbedtls_sha1_context m_ctx;
};
}  // namespace

ShaDevice::ShaDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

std::optional<IPCReply> ShaDevice::Open(const OpenRequest& request)
{
  return Device::Open(request);
}

// in[0] = data to hash (ignored by InitState), io[0] = state block, io[1] = digest output.
HLE::ReturnCode ShaDevice::ProcessShaCommand(ShaIoctlv command, const IOCtlVRequest& request)
{
  auto& memory = GetSystem().GetMemory();
  const u32 state_address = request.io_vectors[0].address;

  ShaContext saved;
  memory.CopyFromEmu(&saved, state_address, sizeof(saved));
  Sha1Engine engine(saved);

  std::array<u8, 20> digest{};
  int status;
  if (command == ShaIoctlv::InitState)
  {
    status = mbedtls_sha1_starts_ret(engine.get());
  }
  else
  {
    const auto& data_in = request.in_vectors[0];
    std::vector<u8> data(data_in.size);
    memory.CopyFromEmu(data.data(), data_in.address, data.size());
    status = mbedtls_sha1_update_ret(engine.get(), data.data(), data.size());
    if (status == 0 && command == ShaIoctlv::FinalizeState)
      status = mbedtls_sha1_finish_ret(engine.get(), digest.data());
  }

  const ShaContext updated = engine.Save();
  memory.CopyToEmu(state_address, &updated, sizeof(updated));

  const bool failed = status != 0;
  if (!failed && command == ShaIoctlv::FinalizeState)
    memory.CopyToEmu(request.io_vectors[1].address, digest.data(), digest.size());

  return failed ? HLE::ReturnCode::IPC_EACCES : HLE::ReturnCode::IPC_SUCCESS;
}

std::optional<IPCReply> ShaDevice::IOCtlV(const IOCtlVRequest& request)
{
  const auto command = static_cast<ShaIoctlv>(request.request);
  const bool known = command == ShaIoctlv::InitState || command == ShaIoctlv::ContributeState ||
                     command == ShaIoctlv::FinalizeState;
  if (!known || !request.HasNumberOfValidVectors(1, 2))
    return IPCReply(IPC_EINVAL);

  return IPCReply(ProcessShaCommand(command, request));
}
}  // namespace IOS::HLE
