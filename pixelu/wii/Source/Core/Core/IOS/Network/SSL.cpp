// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/SSL.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace IOS::HLE
{
WII_SSL NetSSLDevice::_SSL[NET_SSL_MAXINSTANCES];

namespace
{
// Certificate checks as loose as the Wii's: SHA-1 or better, any key type and curve, and no
// minimum RSA key size.
constexpr mbedtls_x509_crt_profile WII_CERT_PROFILE = {
    MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA1) | MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_RIPEMD160) |
        MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA224) | MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA256) |
        MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA384) | MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA512),
    0xFFFFFFF,
    0xFFFFFFF,
    0,
};

// The Wii does not send Server Name Indication, and mbedtls always sends it once a hostname
// is set. The hostname is still needed to check the certificate's common name, so it is only
// set once the Client Hello has gone out: mbedtls flushes its output (and so calls this) at
// the start of every handshake step, and the step after the Client Hello waits for the
// Server Hello.
int SendWithoutSNI(void* context, const unsigned char* buf, size_t len)
{
  WII_SSL& ssl = *static_cast<WII_SSL*>(context);
  if (ssl.ctx.state == MBEDTLS_SSL_SERVER_HELLO)
    mbedtls_ssl_set_hostname(&ssl.ctx, ssl.hostname.c_str());

  const int sent = mbedtls_net_send(&ssl.hostfd, buf, len);
  // Encrypted traffic is captured only when the plain text is not.
  if (sent > 0 && !Config::Get(Config::MAIN_NETWORK_SSL_DUMP_WRITE))
  {
    Core::System::GetInstance().GetPowerPC().GetDebugInterface().NetworkLogger()->LogWrite(
        buf, static_cast<size_t>(sent), ssl.hostfd, nullptr);
  }
  return sent;
}

int Receive(void* context, unsigned char* buf, size_t len)
{
  WII_SSL& ssl = *static_cast<WII_SSL*>(context);
  const int received = mbedtls_net_recv(&ssl.hostfd, buf, len);
  if (received > 0 && !Config::Get(Config::MAIN_NETWORK_SSL_DUMP_READ))
  {
    Core::System::GetInstance().GetPowerPC().GetDebugInterface().NetworkLogger()->LogRead(
        buf, static_cast<size_t>(received), ssl.hostfd, nullptr);
  }
  return received;
}

// Ends a context. The client key is not freed here.
void CloseContext(WII_SSL& ssl)
{
  mbedtls_ssl_close_notify(&ssl.ctx);
  mbedtls_x509_crt_free(&ssl.cacert);
  mbedtls_x509_crt_free(&ssl.clicert);
  mbedtls_ssl_session_free(&ssl.session);
  mbedtls_ssl_free(&ssl.ctx);
  mbedtls_ssl_config_free(&ssl.config);
  mbedtls_ctr_drbg_free(&ssl.ctr_drbg);
  mbedtls_entropy_free(&ssl.entropy);
  ssl.hostname.clear();
  ssl.active = false;
}

// Sets up the context in a free slot. False if the random generator could not be seeded.
bool OpenContext(WII_SSL& ssl, bool verify, const std::string& hostname)
{
  mbedtls_ssl_init(&ssl.ctx);
  mbedtls_entropy_init(&ssl.entropy);
  mbedtls_ctr_drbg_init(&ssl.ctr_drbg);
  static constexpr char PERSONALIZATION[] = "dolphin-emu";
  if (mbedtls_ctr_drbg_seed(&ssl.ctr_drbg, mbedtls_entropy_func, &ssl.entropy,
                            reinterpret_cast<const unsigned char*>(PERSONALIZATION),
                            std::strlen(PERSONALIZATION)) != 0)
  {
    mbedtls_ssl_free(&ssl.ctx);
    mbedtls_ctr_drbg_free(&ssl.ctr_drbg);
    mbedtls_entropy_free(&ssl.entropy);
    return false;
  }

  mbedtls_ssl_config* const config = &ssl.config;
  mbedtls_ssl_config_init(config);
  mbedtls_ssl_config_defaults(config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                              MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_rng(config, mbedtls_ctr_drbg_random, &ssl.ctr_drbg);
  // Handshakes fail with TLS 1.2; up to TLS 1.1 works.
  mbedtls_ssl_conf_max_version(config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_2);
  mbedtls_ssl_conf_cert_profile(config, &WII_CERT_PROFILE);
  mbedtls_ssl_set_session(&ssl.ctx, &ssl.session);
  mbedtls_ssl_conf_authmode(config, verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_renegotiation(config, MBEDTLS_SSL_RENEGOTIATION_ENABLED);

  ssl.hostname = hostname;
  ssl.active = true;
  return true;
}
}  // namespace

NetSSLDevice::NetSSLDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
  for (WII_SSL& ssl : _SSL)
    ssl.active = false;
}

NetSSLDevice::~NetSSLDevice()
{
  for (WII_SSL& ssl : _SSL)
  {
    if (ssl.active)
      CloseContext(ssl);
  }
}

int NetSSLDevice::GetSSLFreeID() const
{
  for (int slot = 0; slot < NET_SSL_MAXINSTANCES; ++slot)
  {
    if (!_SSL[slot].active)
      return slot + 1;
  }
  return 0;
}

std::optional<IPCReply> NetSSLDevice::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_SSL, Common::Log::LogLevel::LINFO);
  return IPCReply(IPC_SUCCESS);
}

constexpr std::array<u8, 32> s_client_cert_hash = {
    {0x22, 0x9e, 0xc6, 0x78, 0x52, 0x5e, 0x06, 0x05, 0x88, 0xe8, 0xea,
     0x23, 0xe5, 0x45, 0x9e, 0xc1, 0x4a, 0xf3, 0xc2, 0xeb, 0xb7, 0xb9,
     0xe6, 0x9e, 0xc4, 0x6b, 0x0f, 0xaf, 0x01, 0x17, 0x30, 0xd9}};
constexpr std::array<u8, 32> s_client_key_hash = {{0x72, 0x3b, 0xe9, 0xb3, 0x2c, 0x3a, 0xfb, 0x83,
                                                   0xa4, 0xa3, 0x75, 0x7a, 0xdf, 0x35, 0x25, 0x29,
                                                   0xe9, 0x0c, 0x0a, 0xd6, 0xfa, 0xd5, 0x25, 0x09,
                                                   0x96, 0x3b, 0xa8, 0x94, 0x2a, 0xe6, 0x25, 0xdf}};
constexpr std::array<u8, 32> s_root_ca_hash = {{0xc5, 0xb0, 0xf8, 0xdf, 0xce, 0xc6, 0xb9, 0xed,
                                                0x2a, 0xc3, 0x8b, 0x8b, 0xc6, 0x9a, 0x4d, 0xb7,
                                                0xc2, 0x09, 0xdc, 0x17, 0x7d, 0x24, 0x3c, 0x8d,
                                                0xf2, 0xbd, 0xdf, 0x9e, 0x39, 0x17, 0x1e, 0x5f}};

namespace
{
// Loads one of the user-supplied certificate files and checks its SHA-256. Empty if it is
// missing or not the expected file; unless `silent`, the user is told how to set it up.
std::vector<u8> LoadCheckedFile(const std::string& path, const std::array<u8, 32>& expected,
                                bool silent)
{
  File::IOFile file(path, "rb");
  std::vector<u8> contents(file.GetSize());
  if (!file.ReadBytes(contents.data(), contents.size()))
  {
    ERROR_LOG_FMT(IOS_SSL, "Could not read {}", path);
    if (!silent)
    {
      PanicAlertFmtT("IOS: Could not read a file required for SSL services ({0}). Please refer to "
                     "https://dolphin-emu.org/docs/guides/wii-network-guide/ for "
                     "instructions on setting up Wii networking.",
                     path);
    }
    return {};
  }

  std::array<u8, 32> digest;
  mbedtls_sha256_ret(contents.data(), contents.size(), digest.data(), 0);
  if (digest == expected)
    return contents;

  ERROR_LOG_FMT(IOS_SSL, "{} does not have the expected hash", path);
  if (!silent)
  {
    PanicAlertFmtT("IOS: A file required for SSL services ({0}) is invalid. Please refer to "
                   "https://dolphin-emu.org/docs/guides/wii-network-guide/ for "
                   "instructions on setting up Wii networking.",
                   path);
  }
  return {};
}

// The first three vectors of each kind, zero where the request has fewer.
struct SSLVectors
{
  explicit SSLVectors(const IOCtlVRequest& request)
  {
    for (size_t i = 0; i < 3; ++i)
    {
      if (i < request.in_vectors.size())
        in[i] = request.in_vectors[i];
      if (i < request.io_vectors.size())
        out[i] = request.io_vectors[i];
    }
  }

  std::array<IOCtlVRequest::IOVector, 3> in{};
  std::array<IOCtlVRequest::IOVector, 3> out{};
};

void LogVectors(std::string_view name, const SSLVectors& v)
{
  INFO_LOG_FMT(IOS_SSL,
               "{} in ({:08x}, {}), in2 ({:08x}, {}), in3 ({:08x}, {}), out ({:08x}, {}), "
               "out2 ({:08x}, {}), out3 ({:08x}, {})",
               name, v.in[0].address, v.in[0].size, v.in[1].address, v.in[1].size, v.in[2].address,
               v.in[2].size, v.out[0].address, v.out[0].size, v.out[1].address, v.out[1].size,
               v.out[2].address, v.out[2].size);
}
}  // namespace

std::optional<IPCReply> NetSSLDevice::IOCtlV(const IOCtlVRequest& request)
{
  const SSLVectors v{request};

  // SSL traffic cannot be replayed, so it is refused while determinism is needed.
  if (Core::WantsDeterminism())
    return IPCReply(IPC_EACCES);

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  // Every command reports its SSL result in the first input vector and the IPC call itself
  // succeeds. Most take the context ID in the first output vector.
  const u32 result_addr = v.in[0].address;
  const auto report = [&](s32 code) { WriteReturnValue(memory, code, result_addr); };
  const auto context_id = [&] { return static_cast<int>(memory.Read_U32(v.out[0].address) - 1); };

  switch (request.request)
  {
  case IOCTLV_NET_SSL_NEW:
  {
    const int verify = static_cast<int>(memory.Read_U32(v.out[0].address));
    const std::string hostname = memory.GetString(v.out[1].address, v.out[1].size);

    const int id = GetSSLFreeID();
    const bool check_certificates =
        Config::Get(Config::MAIN_NETWORK_SSL_VERIFY_CERTIFICATES) && verify != 0;
    if (id != 0 && OpenContext(_SSL[id - 1], check_certificates, hostname))
      report(id);
    else
      report(SSL_ERR_FAILED);

    INFO_LOG_FMT(IOS_SSL, "IOCTLV_NET_SSL_NEW ({}, {})", verify, hostname);
    LogVectors("IOCTLV_NET_SSL_NEW", v);
    break;
  }

  case IOCTLV_NET_SSL_SHUTDOWN:
  {
    const int ssl_id = context_id();
    if (IsSSLIDValid(ssl_id))
    {
      CloseContext(_SSL[ssl_id]);
      report(SSL_OK);
    }
    else
    {
      report(SSL_ERR_ID);
    }
    LogVectors("IOCTLV_NET_SSL_SHUTDOWN", v);
    break;
  }

  case IOCTLV_NET_SSL_SETROOTCA:
  {
    LogVectors("IOCTLV_NET_SSL_SETROOTCA", v);
    const int ssl_id = context_id();
    if (!IsSSLIDValid(ssl_id))
    {
      report(SSL_ERR_ID);
      break;
    }

    WII_SSL& ssl = _SSL[ssl_id];
    // The CA certificate (DER) is in the second output vector.
    const IOCtlVRequest::IOVector& der = v.out[1];
    const int ret = mbedtls_x509_crt_parse_der(
        &ssl.cacert, memory.GetPointerForRange(der.address, der.size), der.size);

    if (Config::Get(Config::MAIN_NETWORK_SSL_DUMP_ROOT_CA))
    {
      File::IOFile(File::GetUserPath(D_DUMPSSL_IDX) + ssl.hostname + "_rootca.der", "wb")
          .WriteBytes(memory.GetPointerForRange(der.address, der.size), der.size);
    }

    if (ret == 0)
    {
      mbedtls_ssl_conf_ca_chain(&ssl.config, &ssl.cacert, nullptr);
      report(SSL_OK);
    }
    else
    {
      report(SSL_ERR_FAILED);
    }
    INFO_LOG_FMT(IOS_SSL, "IOCTLV_NET_SSL_SETROOTCA = {}", ret);
    break;
  }

  case IOCTLV_NET_SSL_SETBUILTINCLIENTCERT:
  {
    LogVectors("IOCTLV_NET_SSL_SETBUILTINCLIENTCERT", v);
    const int ssl_id = context_id();
    if (!IsSSLIDValid(ssl_id))
    {
      report(SSL_ERR_ID);
      INFO_LOG_FMT(IOS_SSL, "IOCTLV_NET_SSL_SETBUILTINCLIENTCERT: bad context {}", ssl_id);
      break;
    }

    WII_SSL& ssl = _SSL[ssl_id];
    const std::string folder = File::GetUserPath(D_SESSION_WIIROOT_IDX);
    const std::vector<u8> cert =
        LoadCheckedFile(folder + "/clientca.pem", s_client_cert_hash, m_cert_error_shown);
    const std::vector<u8> key =
        LoadCheckedFile(folder + "/clientcakey.pem", s_client_key_hash, m_cert_error_shown);
    if (cert.empty() || key.empty())
      m_cert_error_shown = true;

    const int cert_ret = mbedtls_x509_crt_parse(&ssl.clicert, cert.data(), cert.size());
    const int key_ret = mbedtls_pk_parse_key(&ssl.pk, key.data(), key.size(), nullptr, 0);
    if (cert_ret == 0 && key_ret == 0)
    {
      mbedtls_ssl_conf_own_cert(&ssl.config, &ssl.clicert, &ssl.pk);
      report(SSL_OK);
    }
    else
    {
      mbedtls_x509_crt_free(&ssl.clicert);
      mbedtls_pk_free(&ssl.pk);
      report(SSL_ERR_FAILED);
    }
    INFO_LOG_FMT(IOS_SSL, "IOCTLV_NET_SSL_SETBUILTINCLIENTCERT = ({}, {})", cert_ret, key_ret);
    break;
  }

  case IOCTLV_NET_SSL_REMOVECLIENTCERT:
  {
    LogVectors("IOCTLV_NET_SSL_REMOVECLIENTCERT", v);
    const int ssl_id = context_id();
    if (!IsSSLIDValid(ssl_id))
    {
      report(SSL_ERR_ID);
      INFO_LOG_FMT(IOS_SSL, "IOCTLV_NET_SSL_REMOVECLIENTCERT: bad context {}", ssl_id);
      break;
    }

    WII_SSL& ssl = _SSL[ssl_id];
    mbedtls_x509_crt_free(&ssl.clicert);
    mbedtls_pk_free(&ssl.pk);
    mbedtls_ssl_conf_own_cert(&ssl.config, nullptr, nullptr);
    report(SSL_OK);
    break;
  }

  case IOCTLV_NET_SSL_SETBUILTINROOTCA:
  {
    const int ssl_id = context_id();
    if (IsSSLIDValid(ssl_id))
    {
      WII_SSL& ssl = _SSL[ssl_id];
      const std::vector<u8> root_ca =
          LoadCheckedFile(File::GetUserPath(D_SESSION_WIIROOT_IDX) + "/rootca.pem",
                          s_root_ca_hash, m_cert_error_shown);
      if (root_ca.empty())
        m_cert_error_shown = true;

      const int ret = mbedtls_x509_crt_parse(&ssl.cacert, root_ca.data(), root_ca.size());
      if (ret == 0)
      {
        mbedtls_ssl_conf_ca_chain(&ssl.config, &ssl.cacert, nullptr);
        report(SSL_OK);
      }
      else
      {
        // Long-standing behaviour: the client certificate (not the CA) is dropped on failure.
        mbedtls_x509_crt_free(&ssl.clicert);
        report(SSL_ERR_FAILED);
      }
      INFO_LOG_FMT(IOS_SSL, "IOCTLV_NET_SSL_SETBUILTINROOTCA = {}", ret);
    }
    else
    {
      report(SSL_ERR_ID);
    }
    LogVectors("IOCTLV_NET_SSL_SETBUILTINROOTCA", v);
    break;
  }

  case IOCTLV_NET_SSL_CONNECT:
  {
    const int ssl_id = context_id();
    if (IsSSLIDValid(ssl_id))
    {
      WII_SSL& ssl = _SSL[ssl_id];
      mbedtls_ssl_setup(&ssl.ctx, &ssl.config);
      // The Wii socket to run over is in the second output vector.
      ssl.sockfd = static_cast<int>(memory.Read_U32(v.out[1].address));
      ssl.hostfd = GetEmulationKernel().GetSocketManager()->GetHostSocket(ssl.sockfd);
      INFO_LOG_FMT(IOS_SSL, "IOCTLV_NET_SSL_CONNECT socket = {}", ssl.sockfd);
      mbedtls_ssl_set_bio(&ssl.ctx, &ssl, SendWithoutSNI, Receive, nullptr);
      report(SSL_OK);
    }
    else
    {
      report(SSL_ERR_ID);
    }
    LogVectors("IOCTLV_NET_SSL_CONNECT", v);
    break;
  }

  case IOCTLV_NET_SSL_DOHANDSHAKE:
  case IOCTLV_NET_SSL_WRITE:
  case IOCTLV_NET_SSL_READ:
  {
    // These can block, so they run on the socket and are answered from there.
    const int ssl_id = context_id();
    if (IsSSLIDValid(ssl_id))
    {
      GetEmulationKernel().GetSocketManager()->DoSock(_SSL[ssl_id].sockfd, request,
                                                      static_cast<SSL_IOCTL>(request.request));
      return std::nullopt;
    }

    report(SSL_ERR_ID);
    if (request.request == IOCTLV_NET_SSL_WRITE)
    {
      LogVectors("IOCTLV_NET_SSL_WRITE", v);
      INFO_LOG_FMT(IOS_SSL, "{}", memory.GetString(v.out[1].address));
    }
    else if (request.request == IOCTLV_NET_SSL_READ)
    {
      LogVectors("IOCTLV_NET_SSL_READ", v);
    }
    break;
  }

  case IOCTLV_NET_SSL_SETROOTCADEFAULT:
  case IOCTLV_NET_SSL_SETCLIENTCERTDEFAULT:
    // Accepted for any open context; nothing to set up.
    report(IsSSLIDValid(context_id()) ? SSL_OK : SSL_ERR_ID);
    LogVectors(request.request == IOCTLV_NET_SSL_SETROOTCADEFAULT ?
                   "IOCTLV_NET_SSL_SETROOTCADEFAULT" :
                   "IOCTLV_NET_SSL_SETCLIENTCERTDEFAULT",
               v);
    break;

  default:
    request.DumpUnknown(system, GetDeviceName(), Common::Log::LogType::IOS_SSL);
    break;
  }

  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
