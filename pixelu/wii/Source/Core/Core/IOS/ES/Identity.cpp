// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// ES calls about the console's identity: device ID and certificate, signing, signature checks,
// and AES with IOSC keys.

#include "Core/IOS/ES/ES.h"

#include <utility>
#include <vector>

#include "Common/Crypto/SHA1.h"
#include "Common/Crypto/ec.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"

namespace IOS::HLE
{
ReturnCode ESCore::GetDeviceId(u32* device_id) const
{
  const u32 id = m_ios.GetIOSC().GetDeviceId();
  INFO_LOG_FMT(IOS_ES, "GetDeviceId: {:08X}", id);
  *device_id = id;
  return IPC_SUCCESS;
}

IPCReply ESDevice::GetDeviceId(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  u32 device_id;
  if (const ReturnCode ret = m_core.GetDeviceId(&device_id); ret != IPC_SUCCESS)
    return IPCReply(ret);
  GetSystem().GetMemory().Write_U32(device_id, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

namespace
{
// Encrypt and Decrypt take: in = {key handle, (unused), data}; io = {IV (updated), output}.
// Whether the caller may use the key is left to IOSC's ownership check.
struct CryptBuffers
{
  u32 key_handle;
  u32 size;
  const u8* input;
  u8* iv;
  u8* output;
};

CryptBuffers GetCryptBuffers(Memory::MemoryManager& memory, const IOCtlVRequest& request)
{
  CryptBuffers buffers;
  buffers.key_handle = memory.Read_U32(request.in_vectors[0].address);
  buffers.size = request.in_vectors[2].size;
  buffers.input = memory.GetPointerForRange(request.in_vectors[2].address, buffers.size);
  buffers.iv = memory.GetPointerForRange(request.io_vectors[0].address, 16);
  buffers.output = memory.GetPointerForRange(request.io_vectors[1].address, buffers.size);
  return buffers;
}
}  // namespace

IPCReply ESDevice::Encrypt(u32 /*uid*/, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 2))
    return IPCReply(ES_EINVAL);

  const CryptBuffers b = GetCryptBuffers(GetSystem().GetMemory(), request);
  return IPCReply(GetEmulationKernel().GetIOSC().Encrypt(b.key_handle, b.iv, b.input, b.size,
                                                         b.output, PID_ES));
}

IPCReply ESDevice::Decrypt(u32 /*uid*/, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 2))
    return IPCReply(ES_EINVAL);

  const CryptBuffers b = GetCryptBuffers(GetSystem().GetMemory(), request);
  return IPCReply(GetEmulationKernel().GetIOSC().Decrypt(b.key_handle, b.iv, b.input, b.size,
                                                         b.output, PID_ES));
}

IPCReply ESDevice::CheckKoreaRegion(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return IPCReply(ES_EINVAL);

  // IOS70 provides this for system menu 4.2, which uses it to detect region-changed Korean
  // consoles: success means the Korean key was found (and leads to error 003). Report that no
  // Korean key is present.
  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_CHECKKOREAREGION: Title checked for Korean keys.");
  return IPCReply(ES_EINVAL);
}

IPCReply ESDevice::GetDeviceCertificate(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != sizeof(CertECC))
    return IPCReply(ES_EINVAL);

  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_GETDEVICECERT");
  const CertECC cert = GetEmulationKernel().GetIOSC().GetDeviceCertificate();
  GetSystem().GetMemory().CopyToEmu(request.io_vectors[0].address, &cert, sizeof(cert));
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::Sign(const IOCtlVRequest& request)
{
  // in: data; io: signature, AP certificate
  if (!request.HasNumberOfValidVectors(1, 2))
    return IPCReply(ES_EINVAL);

  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_SIGN");
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  u8* cert_out = memory.GetPointerForRange(request.io_vectors[1].address, sizeof(CertECC));
  const u32 size = request.in_vectors[0].size;
  const u8* data = memory.GetPointerForRange(request.in_vectors[0].address, size);
  u8* signature_out =
      memory.GetPointerForRange(request.io_vectors[0].address, sizeof(Common::ec::Signature));

  // The AP certificate is made for the running title, so there has to be one.
  if (!m_core.m_title_context.active)
    return IPCReply(ES_EINVAL);

  GetEmulationKernel().GetIOSC().Sign(signature_out, cert_out,
                                      m_core.m_title_context.tmd.GetTitleId(), data, size);
  return IPCReply(IPC_SUCCESS);
}

ReturnCode ESCore::VerifySign(const std::vector<u8>& hash, const std::vector<u8>& ecc_signature,
                              const std::vector<u8>& certs_bytes)
{
  // The chain must hold an AP certificate and the console (NG) certificate that signed it.
  const std::map<std::string, ES::CertReader> certs = ES::ParseCertChain(certs_bytes);
  if (certs.empty())
    return ES_EINVAL;

  const ES::CertReader* ap = nullptr;
  for (const auto& [name, cert] : certs)
  {
    if (name.size() > 2 && name.starts_with("AP"))
    {
      ap = &cert;
      break;
    }
  }
  if (ap == nullptr)
    return ES_UNKNOWN_ISSUER;

  const std::vector<std::string> ap_chain = SplitString(ap->GetIssuer(), '-');
  if (ap_chain.size() < 2)
    return ES_UNKNOWN_ISSUER;
  const auto ng_it = certs.find(ap_chain.back());
  if (ng_it == certs.end())
    return ES_UNKNOWN_ISSUER;
  const ES::CertReader& ng = ng_it->second;

  IOSC& iosc = m_ios.GetIOSC();

  // NG key, verified up to the root.
  IOSC::Handle ng_key;
  ReturnCode ret =
      iosc.CreateObject(&ng_key, IOSC::TYPE_PUBLIC_KEY, IOSC::ObjectSubType::ECC233, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;
  Common::ScopeGuard free_ng{[&] { iosc.DeleteObject(ng_key, PID_ES); }};
  ret = VerifyContainer(VerifyContainerType::Device, VerifyMode::DoNotUpdateCertStore, ng,
                        certs_bytes, ng_key);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "VerifySign: VerifyContainer(ng) failed with error {}",
                  std::to_underlying(ret));
    return ret;
  }

  // The AP certificate, signed by the NG key.
  ret = iosc.VerifyPublicKeySign(ap->GetSha1(), ng_key, ap->GetSignatureData(), PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "VerifySign: IOSC_VerifyPublicKeySign(ap) failed with error {}",
                  std::to_underlying(ret));
    return ret;
  }

  // And the data, signed by the AP key.
  IOSC::Handle ap_key;
  ret = iosc.CreateObject(&ap_key, IOSC::TYPE_PUBLIC_KEY, IOSC::ObjectSubType::ECC233, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;
  Common::ScopeGuard free_ap{[&] { iosc.DeleteObject(ap_key, PID_ES); }};
  ret = iosc.ImportPublicKey(ap_key, ap->GetPublicKey().data(), nullptr, PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "VerifySign: IOSC_ImportPublicKey(ap) failed with error {}",
                  std::to_underlying(ret));
    return ret;
  }

  ret =
      iosc.VerifyPublicKeySign(Common::SHA1::CalculateDigest(hash), ap_key, ecc_signature, PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "VerifySign: IOSC_VerifyPublicKeySign(data) failed with error {}",
                  std::to_underlying(ret));
    return ret;
  }

  INFO_LOG_FMT(IOS_ES, "VerifySign: all checks passed");
  return IPC_SUCCESS;
}

IPCReply ESDevice::VerifySign(const IOCtlVRequest& request)
{
  // in: data, ECC signature, certificate chain
  if (!request.HasNumberOfValidVectors(3, 0) ||
      request.in_vectors[1].size != sizeof(Common::ec::Signature))
  {
    return IPCReply(ES_EINVAL);
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const auto copy_in = [&memory](const IOCtlVRequest::IOVector& vec) {
    std::vector<u8> bytes(vec.size);
    memory.CopyFromEmu(bytes.data(), vec.address, bytes.size());
    return bytes;
  };
  const std::vector<u8> data = copy_in(request.in_vectors[0]);
  const std::vector<u8> signature = copy_in(request.in_vectors[1]);
  const std::vector<u8> certs = copy_in(request.in_vectors[2]);
  return IPCReply(m_core.VerifySign(data, signature, certs));
}
}  // namespace IOS::HLE
