// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Crypto/AesDevice.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
using Block = std::array<u8, 0x10>;

bool IsWordAlignedBlock(const IOCtlVRequest::IOVector& v)
{
  return v.size == sizeof(Block) && (v.address % 4) == 0;
}

std::vector<u8> ReadVector(Memory::MemoryManager& memory, const IOCtlVRequest::IOVector& v)
{
  std::vector<u8> bytes(v.size);
  memory.CopyFromEmu(bytes.data(), v.address, bytes.size());
  return bytes;
}

// in[0] = data, io[0] = output; copies in[0]'s length to io[0] unchanged.
bool CopyData(Memory::MemoryManager& memory, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return false;

  const std::vector<u8> data = ReadVector(memory, request.in_vectors[0]);
  memory.CopyToEmu(request.io_vectors[0].address, data.data(), data.size());
  return true;
}

// in[0] = data, in[1] = key, io[0] = output, io[1] = IV (read, then updated for chaining).
// The output vector is written in full; bytes past the input's length come out as zero.
bool CryptData(Memory::MemoryManager& memory, const IOCtlVRequest& request, bool encrypt)
{
  if (!request.HasNumberOfValidVectors(2, 2))
    return false;

  const auto& data_in = request.in_vectors[0];
  const auto& key_in = request.in_vectors[1];
  const auto& data_out = request.io_vectors[0];
  const auto& iv_io = request.io_vectors[1];
  if (!IsWordAlignedBlock(key_in) || !IsWordAlignedBlock(iv_io))
    return false;

  std::vector<u8> plain = ReadVector(memory, data_in);
  std::vector<u8> result(data_out.size);
  Block key{};
  Block iv{};
  memory.CopyFromEmu(key.data(), key_in.address, key.size());
  memory.CopyFromEmu(iv.data(), iv_io.address, iv.size());

  const auto cipher = encrypt ? Common::AES::CreateContextEncrypt(key.data()) :
                                Common::AES::CreateContextDecrypt(key.data());
  const size_t length = std::min(plain.size(), result.size());
  cipher->Crypt(iv.data(), iv.data(), plain.data(), result.data(), length);

  memory.CopyToEmu(data_out.address, result.data(), result.size());
  memory.CopyToEmu(iv_io.address, iv.data(), iv.size());
  return true;
}
}  // namespace

AesDevice::AesDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

std::optional<IPCReply> AesDevice::Open(const OpenRequest& request)
{
  return Device::Open(request);
}

std::optional<IPCReply> AesDevice::IOCtlV(const IOCtlVRequest& request)
{
  auto& memory = GetSystem().GetMemory();

  bool ok = false;
  const auto command = static_cast<AesIoctlv>(request.request);
  if (command == AesIoctlv::Copy)
    ok = CopyData(memory, request);
  else if (command == AesIoctlv::Encrypt || command == AesIoctlv::Decrypt)
    ok = CryptData(memory, request, command == AesIoctlv::Encrypt);

  return IPCReply(ok ? IPC_SUCCESS : IPC_EINVAL);
}
}  // namespace IOS::HLE
